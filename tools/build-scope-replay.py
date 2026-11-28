import argparse
import csv
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
POINTS = ROOT / 'examples' / 'bench' / '2026-11-28' / 'display-points.csv'
LINKER = '''ENTRY(scope_replay)
SECTIONS {
    . = 0x24000000;
    .text : { KEEP(*(.entry)) *(.text*) *(.rodata*) }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame*) }
}
'''


def load_codes(path):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        points = list(csv.DictReader(stream))
    if [int(point['Frequency_MHz']) for point in points] != list(range(1, 41)):
        raise ValueError('Expected one point per MHz, ordered from 1 to 40 MHz')
    codes = []
    for index, point in enumerate(points):
        detector = float(point['DisplayedDetector_mV'])
        code = int(point['DAC_Code'])
        if not 210 <= detector <= 217 or int(point['TimeFromTrigger_ms']) != index:
            raise ValueError(f'Invalid point at {index + 1} MHz')
        expected = round((.35 + (detector - 210) * 2.45 / 7) * 4095 / 3.3)
        if code != expected or not 0 <= code <= 4095:
            raise ValueError(f'Incorrect DAC mapping at {index + 1} MHz')
        codes.append(code)
    return codes


def gdb_script(codes, port):
    return f'''set pagination off
set confirm off
set remotetimeout 5
target extended-remote 127.0.0.1:{port}
printf "Original PC=0x%x PSR=0x%x\\n", $pc, $xpsr
if ($xpsr & 0x1ff) != 0 || $pc < 0x08000000 || $pc >= 0x08100000
    detach
    quit 2
end
restore replay.bin binary 0x24000000
hbreak frame_ready
set $pc=0x24000000
continue
x/8wx 0x24001000
if *(unsigned int *)0x24001004 != 0x600d0001 || *(unsigned int *)0x2400100c != {codes[0]} || *(unsigned int *)0x24001010 != {codes[-1]} || *(unsigned int *)0x24001014 != 192 || *(unsigned int *)0x24001018 < 2560000 || *(unsigned int *)0x24001018 > 2561000 || *(unsigned int *)0x2400101c < 3200000 || *(unsigned int *)0x2400101c > 3201000
    monitor reset
    detach
    quit 3
end
continue
x/8wx 0x24001000
if *(unsigned int *)0x24001008 != 2 || *(unsigned int *)0x24001004 != 0x600d0001
    monitor reset
    detach
    quit 4
end
delete breakpoints
detach
'''


def main():
    parser = argparse.ArgumentParser(description='Build the 2017H 40-point RAM replay without connecting a board')
    parser.add_argument('--cube-clt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--port', type=int, default=61234)
    options = parser.parse_args()
    if not 1024 <= options.port <= 65535:
        parser.error('--port must be between 1024 and 65535')
    try:
        codes = load_codes(POINTS)
        output = options.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
        header = '#include <stdint.h>\nstatic const uint16_t replay_codes[40] = {\n    '
        header += ', '.join(map(str, codes)) + '\n};\n'
        (output / 'scope_replay_table.h').write_text(header, encoding='ascii')
        (output / 'replay.ld').write_text(LINKER, encoding='ascii')

        compiler_bin = options.cube_clt.resolve() / 'GNU-tools-for-STM32' / 'bin'
        cmsis = ROOT / 'Drivers' / 'CMSIS'
        subprocess.run([
            str(compiler_bin / 'arm-none-eabi-gcc.exe'), '-mcpu=cortex-m7', '-mthumb', '-Os', '-g',
            '-Wall', '-Wextra', '-Werror', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
            '-nostdlib', '-ffreestanding', '-fno-stack-protector', '-I',
            str(cmsis / 'Device' / 'ST' / 'STM32H7xx' / 'Include'), '-I', str(cmsis / 'Include'),
            '-I', str(output), str(ROOT / 'tools' / 'scope_replay.c'), '-T', str(output / 'replay.ld'),
            '-o', str(output / 'replay.elf'),
        ], check=True)
        subprocess.run([
            str(compiler_bin / 'arm-none-eabi-objcopy.exe'), '-O', 'binary',
            str(output / 'replay.elf'), str(output / 'replay.bin'),
        ], check=True)
        if not 0 < (output / 'replay.bin').stat().st_size <= 4096:
            raise ValueError('Replay image overlaps the RAM status block')
        (output / 'replay.gdb').write_text(gdb_script(codes, options.port), encoding='ascii')
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Replay build stopped: {error}\n')
    print(f'40 points, PA4, 50 ms/frame; RAM-only image: {output}')


if __name__ == '__main__':
    main()
