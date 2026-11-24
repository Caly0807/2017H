#include "auto_sweep.h"
#include "ad9959.h"
#include "ads1256.h"
#include "adc_config.h"
#include "dds_config.h"

enum
{
  SWEEP_RUNNING = 1U,
  SWEEP_DONE,
  SWEEP_DDS_INIT_FAILED,
  SWEEP_ADC_INIT_FAILED,
  SWEEP_DDS_WRITE_FAILED,
  SWEEP_ADC_READ_FAILED,
  SWEEP_ADC_TIMEOUT,
#ifdef ENABLE_DETECTOR_TEST
  SWEEP_ADC_CHANNEL = DETECTOR_ADC_CHANNEL,
  SWEEP_SETTLE_MS = 500U,
  SWEEP_SAMPLES = 32U,
  SWEEP_TIMEOUT_MS = 2000U,
  SWEEP_AMPLITUDE = 256U,
  SWEEP_STEP_HZ = DDS_START_FREQUENCY_HZ,
#else
  SWEEP_ADC_CHANNEL = ADC_START_CHANNEL,
  SWEEP_SETTLE_MS = 150U,
  SWEEP_SAMPLES = 16U,
  SWEEP_TIMEOUT_MS = 1000U,
  SWEEP_AMPLITUDE = SWEEP_AMPLITUDE_CODE,
  SWEEP_STEP_HZ = 1000000U,
#endif
  SWEEP_LAST_MHZ = 40U
};

#ifdef ENABLE_DETECTOR_TEST
static const uint16_t test_amplitudes[] = {16U, 64U, 128U, 256U, 256U, 128U,
                                         64U, 16U, 64U, 256U, 16U, 128U};
enum { SWEEP_RECORDS = 2U * sizeof(test_amplitudes) / sizeof(test_amplitudes[0]) + 1U };
#else
enum { SWEEP_RECORDS = SWEEP_LAST_MHZ + 1U };
#endif
_Static_assert(SWEEP_RECORDS <= SWEEP_LAST_MHZ + 1U, "SWD record capacity");

typedef struct
{
  uint32_t stimulus;
  int32_t mean_raw;
  int32_t min_raw;
  int32_t max_raw;
  int32_t mean_input_uv;
  uint32_t sample_span_ms;
} SweepPoint;

typedef struct
{
  uint32_t magic;
  uint32_t status;
  uint32_t point_count;
  uint32_t amplitude_code;
  uint32_t software_ref_uv;
  uint32_t settle_ms;
  uint32_t samples_per_point;
  uint32_t elapsed_ms;
  uint32_t failed_frequency_hz;
  uint32_t adc_rate_setting;
  uint32_t adc_channel;
  uint32_t mute_command_ok;
  SweepPoint points[SWEEP_LAST_MHZ + 1U];
} SweepResult;

_Static_assert(sizeof(SweepPoint) == 24U && sizeof(SweepResult) == 1032U, "SWD reader ABI");
#ifdef ENABLE_DETECTOR_TEST
const uint32_t g_auto_sweep_tag = 0x44544331U;
#else
const uint32_t g_auto_sweep_tag = 0x53575031U;
#endif
volatile SweepResult g_auto_sweep;

static uint32_t Sweep_Collect(ADS1256_HandleTypeDef *adc, SweepPoint *point)
{
  uint32_t start = HAL_GetTick();
  uint32_t count = 0U;
  uint32_t first_tick = 0U;
  uint8_t discarded = 0U;
  int64_t sum = 0;
  int64_t sum_uv = 0;
  while ((uint32_t)(HAL_GetTick() - start) < SWEEP_TIMEOUT_MS)
  {
    HAL_StatusTypeDef result = ADS1256_Poll(adc);
    if (result == HAL_BUSY)
    {
      continue;
    }
    if (result != HAL_OK)
    {
      return SWEEP_ADC_READ_FAILED;
    }
    if ((uint32_t)(adc->sample_tick - start) < SWEEP_SETTLE_MS)
    {
      continue;
    }
    if (!discarded)
    {
      discarded = 1U;
      continue;
    }
    if (count == 0U)
    {
      first_tick = adc->sample_tick;
      point->min_raw = adc->raw;
      point->max_raw = adc->raw;
    }
    if (adc->raw < point->min_raw) { point->min_raw = adc->raw; }
    if (adc->raw > point->max_raw) { point->max_raw = adc->raw; }
    sum += adc->raw;
    sum_uv += adc->voltage_uv;
    if (++count == SWEEP_SAMPLES)
    {
      point->mean_raw = (int32_t)(sum / SWEEP_SAMPLES);
      point->mean_input_uv = (int32_t)(sum_uv / SWEEP_SAMPLES);
      point->sample_span_ms = adc->sample_tick - first_tick;
      return SWEEP_DONE;
    }
  }
  return SWEEP_ADC_TIMEOUT;
}

void AutoSweep_Run(SPI_HandleTypeDef *dds_spi, SPI_HandleTypeDef *adc_spi)
{
  AD9959_HandleTypeDef dds = {0};
  ADS1256_HandleTypeDef adc = {
    .spi = adc_spi, .reference_uv = ADC_REFERENCE_UV, .rate = ADC_START_RATE,
    .channel = SWEEP_ADC_CHANNEL, .gain = ADC_START_GAIN
  };
  uint32_t start = HAL_GetTick();
  uint32_t status = SWEEP_DDS_INIT_FAILED;
  g_auto_sweep.magic = *(const volatile uint32_t *)&g_auto_sweep_tag;
  g_auto_sweep.amplitude_code = SWEEP_AMPLITUDE;
  g_auto_sweep.software_ref_uv = ADC_REFERENCE_UV;
  g_auto_sweep.settle_ms = SWEEP_SETTLE_MS;
  g_auto_sweep.samples_per_point = SWEEP_SAMPLES;
  g_auto_sweep.adc_rate_setting = ADC_START_RATE;
  g_auto_sweep.adc_channel = SWEEP_ADC_CHANNEL;
  g_auto_sweep.status = SWEEP_RUNNING;
  if (AD9959_Init(&dds, dds_spi, DDS_REFERENCE_CLOCK_HZ, DDS_PLL_MULTIPLIER) != HAL_OK)
  {
    goto finish;
  }
  status = SWEEP_ADC_INIT_FAILED;
  if (ADS1256_Init(&adc) != HAL_OK)
  {
    goto finish;
  }
  for (uint32_t index = 0U; index < SWEEP_RECORDS; ++index)
  {
#ifdef ENABLE_DETECTOR_TEST
    uint32_t amplitude = (index & 1U) != 0U ? test_amplitudes[index / 2U] : 0U;
    uint32_t dds_hz = SWEEP_STEP_HZ;
    SweepPoint point = {.stimulus = amplitude};
    g_auto_sweep.failed_frequency_hz = dds_hz;
#else
    uint32_t amplitude = index == 0U ? 0U : SWEEP_AMPLITUDE;
    SweepPoint point = {.stimulus = index * SWEEP_STEP_HZ};
    uint32_t dds_hz = index == 0U ? SWEEP_STEP_HZ : point.stimulus;
    g_auto_sweep.failed_frequency_hz = point.stimulus;
#endif
    if (AD9959_SetTone(&dds, DDS_OUTPUT_CHANNEL, dds_hz, amplitude, 0U, amplitude != 0U) != HAL_OK)
    {
      status = SWEEP_DDS_WRITE_FAILED;
      goto finish;
    }
    status = Sweep_Collect(&adc, &point);
    if (status != SWEEP_DONE)
    {
      goto finish;
    }
    g_auto_sweep.points[index] = point;
    g_auto_sweep.point_count = index + 1U;
  }
  g_auto_sweep.failed_frequency_hz = 0U;

finish:
  if (dds.system_clock_hz != 0U)
  {
    g_auto_sweep.mute_command_ok = AD9959_SetTone(&dds, DDS_OUTPUT_CHANNEL,
                                                   SWEEP_STEP_HZ, 0U, 0U, 0U) == HAL_OK;
  }
  g_auto_sweep.elapsed_ms = HAL_GetTick() - start;
  __DMB();
  g_auto_sweep.status = status;
}
