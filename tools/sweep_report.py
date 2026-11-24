import argparse
import csv
import json
import math
from pathlib import Path


FREQUENCIES = tuple(range(0, 40_000_001, 1_000_000))
SOURCES = {'swd', 'simulation', 'wifi'}
FIELDS = ('Frequency_Hz', 'DDS_Output_Command', 'DDS_Amplitude_Code', 'Mean_RAW', 'Min_RAW',
          'Max_RAW', 'Delta_From_Off_RAW', 'Uncalibrated_Input_uV', 'Sample_Span_ms')


def integer(value):
    if type(value) not in (int, str):
        raise ValueError('Expected an integer ADC field')
    return int(value)


def validate(bundle):
    meta, source_rows = bundle['metadata'], bundle['records']
    if meta['source'] not in SOURCES or meta['status'] != 'complete' or len(source_rows) != 41:
        raise ValueError('Expected a complete sweep with a known source and 41 records')
    if meta['records'] != 41 or meta['tones'] != 40 or meta.get('analogCalibrated') is not False:
        raise ValueError('Invalid raw sweep metadata')
    for key, lower, upper in (('samplesPerRecord', 2, 256), ('adcRateSettingSps', 1, 30_000),
                              ('adcChannel', 0, 7), ('amplitudeCode', 1, 1023),
                              ('softwareRef_uV', 500_000, 3_300_000), ('settle_ms', 1, 10_000),
                              ('elapsed_ms', 1, 600_000)):
        value = meta[key]
        if type(value) is not int or not lower <= value <= upper:
            raise ValueError(f'Invalid metadata value: {key}')
    if type(meta['muteCommandOK']) is not bool:
        raise ValueError('Invalid mute status')
    expected_ms = (meta['samplesPerRecord'] - 1) * 1000 / meta['adcRateSettingSps']
    rows = []
    for index, record in enumerate(source_rows):
        row = {name: (record[name] if name == 'DDS_Output_Command' else integer(record[name])) for name in FIELDS}
        if row['Frequency_Hz'] != FREQUENCIES[index]:
            raise ValueError(f'Missing, duplicate or out-of-order frequency at record {index}')
        if (row['DDS_Output_Command'], row['DDS_Amplitude_Code']) != (
            ('Tone', meta['amplitudeCode']) if index else ('Off', 0)
        ):
            raise ValueError(f'DDS command mismatch at record {index}')
        minimum, mean, maximum = row['Min_RAW'], row['Mean_RAW'], row['Max_RAW']
        if not -8_388_608 <= minimum <= mean <= maximum <= 8_388_607:
            raise ValueError(f'Invalid ADC statistics at record {index}')
        baseline = rows[0]['Mean_RAW'] if index else mean
        if row['Delta_From_Off_RAW'] != mean - baseline:
            raise ValueError(f'Incorrect OFF-relative delta at record {index}')
        span = row['Sample_Span_ms']
        if span < 0 or span > meta['elapsed_ms']:
            raise ValueError(f'Invalid sample span at record {index}')
        row['Quality'] = ('saturated' if minimum == -8_388_608 or maximum == 8_388_607 else
                          'timing' if not expected_ms * 0.8 - 2 <= span <= expected_ms * 1.2 + 2 else 'ok')
        rows.append(row)
    return {'metadata': dict(meta), 'records': rows}


def load_capture(directory):
    meta = json.loads((directory / 'sweep.json').read_text(encoding='utf-8-sig'))
    with (directory / 'sweep.csv').open(newline='', encoding='utf-8-sig') as stream:
        return validate({'metadata': meta, 'records': list(csv.DictReader(stream))})


def write_csv(path, rows):
    with path.open('w', newline='', encoding='utf-8-sig') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def calibration_models(path, metadata, limit):
    grouped = {}
    with path.open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            frequency, volts, raw = int(row['Frequency_Hz']), float(row['Vpp_V']), int(row['Mean_RAW'])
            role = row['Role']
            if (not math.isfinite(volts) or volts < 0 or frequency not in FREQUENCIES[1:] or
                volts > 100 or not -8_388_608 < raw < 8_388_607 or role not in {'fit', 'check'} or
                row['Source'] != ('simulation' if metadata['source'] == 'simulation' else 'bench') or
                int(row['ADC_Channel']) != metadata['adcChannel'] or
                int(row['ADC_Rate_SPS']) != metadata['adcRateSettingSps'] or
                int(row['Software_Ref_uV']) != metadata['softwareRef_uV']):
                raise ValueError(f'Invalid calibration row at {frequency} Hz')
            grouped.setdefault(frequency, {'fit': [], 'check': []})[role].append((volts, raw))
    models = {}
    for frequency, group in grouped.items():
        fit, check = group['fit'], group['check']
        if len({volts for volts, raw in fit}) < 3 or not check:
            raise ValueError(f'{frequency} Hz needs 3 distinct fit levels and an independent check')
        squared = [volts ** 2 for volts, raw in fit]
        mean_x = sum(squared) / len(fit)
        mean_y = sum(raw for volts, raw in fit) / len(fit)
        denominator = sum((value - mean_x) ** 2 for value in squared)
        slope = sum((value - mean_x) * (raw - mean_y) for value, (_, raw) in zip(squared, fit)) / denominator
        offset = mean_y - slope * mean_x
        low, high = math.sqrt(min(squared)), math.sqrt(max(squared))
        if not math.isfinite(slope) or slope <= 0:
            raise ValueError(f'Invalid square-law fit at {frequency} Hz')
        errors = []
        for volts, raw in check:
            if not low < volts < high or volts in {level for level, _ in fit} or raw < offset:
                raise ValueError(f'{frequency} Hz check must be inside the fit range and at a different level')
            errors.append(abs(math.sqrt((raw - offset) / slope) - volts) / volts)
        if max(errors) > limit:
            raise ValueError(f'{frequency} Hz holdout error {max(errors):.1%} exceeds {limit:.1%}')
        models[frequency] = {'offset': offset, 'counts_per_vpp2': slope, 'fit_low_vpp': low,
                             'fit_high_vpp': high, 'check_max_error': max(errors)}
    if not models:
        raise ValueError('Calibration file has no rows')
    return models


def apply_models(rows, models):
    for row in rows:
        frequency = row['Frequency_Hz']
        model = models.get(frequency)
        row.update(Vpp_V='', Calibration='off_baseline' if not frequency else 'missing')
        if not frequency or not model:
            continue
        if row['Quality'] != 'ok':
            row['Calibration'] = row['Quality']
            continue
        squared = (row['Mean_RAW'] - model['offset']) / model['counts_per_vpp2']
        if not model['fit_low_vpp'] ** 2 <= squared <= model['fit_high_vpp'] ** 2:
            row['Calibration'] = 'outside_calibration_range'
            continue
        row.update(Vpp_V=math.sqrt(squared), Calibration='in_range')


def plot(path, rows, source, calibrated):
    field = 'Vpp_V' if calibrated else 'Delta_From_Off_RAW'
    values = [row[field] for row in rows[1:] if row[field] != '']
    if not values:
        return
    low, high = min(values), max(values)
    margin = max((high - low) * 0.08, max(abs(high), 1) * 0.001)
    low, high = low - margin, high + margin
    left, right, top, bottom = 118, 920, 95, 500
    heading = 'Calibrated peak-to-peak voltage' if calibrated else 'Detector response, relative to RF off'
    unit = 'Vpp (V)' if calibrated else 'ADC code delta (counts)'
    origin = 'SIMULATION ONLY - not board data' if source == 'simulation' else f'Source: {source.upper()}'
    elements = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 960 570" width="960" height="570">',
                '<rect width="960" height="570" fill="#fff"/>',
                '<g font-family="Arial, sans-serif" fill="#13293b">',
                f'<text x="118" y="36" font-size="23" font-weight="bold">{heading}</text>',
                f'<text x="118" y="61" font-size="14" fill="#556a77">{origin}</text>',
                f'<text x="118" y="83" font-size="13">{unit}</text>']
    for index in range(6):
        height = bottom - index * (bottom - top) / 5
        value = low + index * (high - low) / 5
        elements += [f'<path d="M {left} {height} H {right}" stroke="#dbe3e7"/>',
                     f'<text x="107" y="{height + 4}" text-anchor="end" font-size="12">{value:.4g}</text>']
    for mhz in (1, 5, 10, 15, 20, 25, 30, 35, 40):
        horizontal = left + (mhz - 1) * (right - left) / 39
        elements += [f'<path d="M {horizontal} {top} V {bottom}" stroke="#eef2f4"/>',
                     f'<text x="{horizontal}" y="521" text-anchor="middle" font-size="12">{mhz}</text>']
    elements += [f'<path d="M {left} {top} V {bottom} H {right}" stroke="#78909c" fill="none"/>',
                 '<text x="519" y="550" text-anchor="middle" font-size="14">Frequency (MHz)</text>']
    previous = None
    for index, row in enumerate(rows[1:]):
        value = row[field]
        if value == '':
            previous = None
            continue
        horizontal = left + index * (right - left) / 39
        vertical = bottom - (value - low) * (bottom - top) / (high - low)
        quality = row['Quality'] == 'ok'
        if previous is not None and quality:
            elements.append(f'<path d="M {previous[0]:.2f} {previous[1]:.2f} L {horizontal:.2f} {vertical:.2f}" '
                            'stroke="#007e79" stroke-width="2" fill="none"/>')
        color = '#007e79' if quality else '#b54720'
        elements.append(f'<circle cx="{horizontal:.2f}" cy="{vertical:.2f}" r="3" fill="{color}"/>')
        previous = (horizontal, vertical) if quality else None
    path.write_text('\n'.join([*elements, '</g></svg>']), encoding='utf-8')


def render(bundle, output, calibration=None, limit=0.05, xy_full_scale=None):
    meta, records = bundle['metadata'], bundle['records']
    models = calibration_models(calibration, meta, limit) if calibration else {}
    apply_models(records, models)
    calibrated = sum(row['Calibration'] == 'in_range' for row in records)
    if xy_full_scale is not None:
        if not math.isfinite(xy_full_scale) or xy_full_scale <= 0:
            raise ValueError('X-Y full scale must be a positive finite Vpp')
        if calibrated != 40 or any(row['Vpp_V'] > xy_full_scale for row in records[1:]):
            raise ValueError('X-Y export needs 40 calibrated points within full scale')
    output.mkdir(parents=True, exist_ok=False)
    write_csv(output / 'report.csv', records)
    summary = {'capture': meta, 'valid_calibrated_tones': calibrated,
               'flagged_acquisition_records': sum(row['Quality'] != 'ok' for row in records),
               'calibration_model_count': len(models), 'models': models,
               'holdout_relative_error_limit': limit, 'xy_vpp_full_scale': xy_full_scale}
    (output / 'report.json').write_text(json.dumps(summary, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    plot(output / 'detector_response.svg', records, meta['source'], False)
    if calibrated:
        plot(output / 'calibrated_vpp.svg', records, meta['source'], True)
    if xy_full_scale is not None:
        axes = [{'Source': meta['source'], 'Frequency_Hz': row['Frequency_Hz'],
                 'X_code_12bit': round(index * 4095 / 39),
                 'Y_code_12bit': round(row['Vpp_V'] * 4095 / xy_full_scale)}
                for index, row in enumerate(records[1:])]
        write_csv(output / 'xy.csv', axes)
    return summary


def main():
    parser = argparse.ArgumentParser(description='Validate and plot a 2017H capture without hardware')
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--calibration', type=Path)
    parser.add_argument('--max-relative-error', type=float, default=0.05)
    parser.add_argument('--xy-full-scale-vpp', type=float)
    options = parser.parse_args()
    if not 0 < options.max_relative_error < 1:
        parser.error('--max-relative-error must be between 0 and 1')
    try:
        result = render(load_capture(options.capture), options.output, options.calibration,
                        options.max_relative_error, options.xy_full_scale_vpp)
    except (OSError, KeyError, TypeError, ValueError, ArithmeticError) as error:
        parser.exit(1, f'Capture rejected: {error}\n')
    print(f"{result['capture']['source']}: 40 tones, {result['flagged_acquisition_records']} acquisition flags, "
          f"{result['valid_calibrated_tones']} calibrated points -> {options.output}")


if __name__ == '__main__':
    main()
