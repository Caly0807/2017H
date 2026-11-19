#include "ads1256.h"

#define ADS1256_TIMEOUT_MS 3000U
#define ADS1256_SPI_TIMEOUT_MS 20U

typedef struct
{
  uint32_t rate;
  uint8_t code;
} ADS1256_Rate;

static const ADS1256_Rate rates[] = {
  {10U, 0x23U}, {15U, 0x33U}, {25U, 0x43U}, {30U, 0x53U},
  {50U, 0x63U}, {60U, 0x72U}, {100U, 0x82U}, {500U, 0x92U},
  {1000U, 0xA1U}, {2000U, 0xB0U}, {3750U, 0xC0U},
  {7500U, 0xD0U}, {15000U, 0xE0U}, {30000U, 0xF0U}
};

static void ADS1256_DelayUs(uint32_t microseconds)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t cycles = (SystemCoreClock / 1000000U) * microseconds;
  while ((uint32_t)(DWT->CYCCNT - start) < cycles)
  {
  }
}

static void ADS1256_Select(void)
{
  HAL_GPIO_WritePin(ADS_CS_GPIO_Port, ADS_CS_Pin, GPIO_PIN_RESET);
  ADS1256_DelayUs(1U);
}

static void ADS1256_Release(void)
{
  ADS1256_DelayUs(2U);
  HAL_GPIO_WritePin(ADS_CS_GPIO_Port, ADS_CS_Pin, GPIO_PIN_SET);
  ADS1256_DelayUs(1U);
}

static HAL_StatusTypeDef ADS1256_Fail(ADS1256_HandleTypeDef *device,
                                     HAL_StatusTypeDef result, const char *error)
{
  device->initialized = 0U;
  device->has_sample = 0U;
  device->error = error;
  return result;
}

static HAL_StatusTypeDef ADS1256_WaitReady(void)
{
  uint32_t start = HAL_GetTick();
  while (HAL_GPIO_ReadPin(ADS_DRDY_GPIO_Port, ADS_DRDY_Pin) != GPIO_PIN_RESET)
  {
    if ((uint32_t)(HAL_GetTick() - start) >= ADS1256_TIMEOUT_MS)
    {
      return HAL_TIMEOUT;
    }
  }
  return HAL_OK;
}

static HAL_StatusTypeDef ADS1256_Command(ADS1256_HandleTypeDef *device,
                                        uint8_t command)
{
  ADS1256_Select();
  HAL_StatusTypeDef result = HAL_SPI_Transmit(device->spi, &command, 1U,
                                             ADS1256_SPI_TIMEOUT_MS);
  ADS1256_Release();
  return result;
}

static HAL_StatusTypeDef ADS1256_ReadRegisters(ADS1256_HandleTypeDef *device,
                                              uint8_t *registers)
{
  uint8_t command[] = {0x10U, 0x03U};
  uint8_t dummy[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};
  ADS1256_Select();
  HAL_StatusTypeDef result = HAL_SPI_Transmit(device->spi, command, sizeof(command),
                                             ADS1256_SPI_TIMEOUT_MS);
  if (result == HAL_OK)
  {
    ADS1256_DelayUs(8U);
    result = HAL_SPI_TransmitReceive(device->spi, dummy, registers, sizeof(dummy),
                                    ADS1256_SPI_TIMEOUT_MS);
  }
  ADS1256_Release();
  return result;
}

static uint8_t ADS1256_RateCode(uint32_t rate, uint8_t *code)
{
  for (uint32_t index = 0U; index < sizeof(rates) / sizeof(rates[0]); ++index)
  {
    if (rates[index].rate == rate)
    {
      *code = rates[index].code;
      return 1U;
    }
  }
  return 0U;
}

static uint8_t ADS1256_GainCode(uint8_t gain, uint8_t *code)
{
  for (uint8_t index = 0U; index <= 6U; ++index)
  {
    if (gain == (1U << index))
    {
      *code = index;
      return 1U;
    }
  }
  return 0U;
}

uint8_t ADS1256_ValidSettings(uint8_t channel, uint8_t gain, uint32_t rate)
{
  uint8_t code;
  return channel <= 7U && ADS1256_GainCode(gain, &code) &&
         ADS1256_RateCode(rate, &code);
}

void ADS1256_SetReference(ADS1256_HandleTypeDef *device, uint32_t reference_uv)
{
  device->reference_uv = reference_uv;
  if (device->has_sample)
  {
    device->voltage_uv = (int32_t)(((int64_t)device->raw * 2 * reference_uv) /
                                  ((int64_t)8388608 * device->gain));
  }
}

HAL_StatusTypeDef ADS1256_Configure(ADS1256_HandleTypeDef *device,
                                  uint8_t channel, uint8_t gain, uint32_t rate)
{
  uint8_t gain_code;
  uint8_t rate_code;
  if (!ADS1256_ValidSettings(channel, gain, rate) || device->spi == NULL)
  {
    return HAL_ERROR;
  }
  ADS1256_GainCode(gain, &gain_code);
  ADS1256_RateCode(rate, &rate_code);
  device->initialized = 0U;
  device->has_sample = 0U;
  if (ADS1256_WaitReady() != HAL_OK)
  {
    return ADS1256_Fail(device, HAL_TIMEOUT, "DRDY timeout; check power/clock/PB0");
  }
  uint8_t configuration[] = {0x50U, 0x03U, 0x00U,
                              (uint8_t)((channel << 4U) | 0x08U), gain_code, rate_code};
  ADS1256_Select();
  HAL_StatusTypeDef result = HAL_SPI_Transmit(device->spi, configuration,
                                             sizeof(configuration), ADS1256_SPI_TIMEOUT_MS);
  ADS1256_Release();
  if (result != HAL_OK)
  {
    return ADS1256_Fail(device, result, "SPI configuration failed; send ADC INIT");
  }
  uint8_t registers[4];
  if (ADS1256_ReadRegisters(device, registers) != HAL_OK)
  {
    return ADS1256_Fail(device, HAL_ERROR, "SPI register read failed; send ADC INIT");
  }
  device->chip_id = registers[0] >> 4U;
  if (device->chip_id != 3U || (registers[0] & 0x0EU) != 0U ||
      registers[1] != configuration[3] || registers[2] != gain_code ||
      registers[3] != rate_code)
  {
    return ADS1256_Fail(device, HAL_ERROR, "register verification failed; check SPI3 wiring");
  }
  if (ADS1256_Command(device, 0xF0U) != HAL_OK)
  {
    return ADS1256_Fail(device, HAL_ERROR, "self-calibration command failed");
  }
  HAL_Delay(1U);
  if (ADS1256_WaitReady() != HAL_OK)
  {
    return ADS1256_Fail(device, HAL_TIMEOUT, "self-calibration timeout");
  }
  device->channel = channel;
  device->gain = gain;
  device->rate = rate;
  device->samples = 0U;
  device->sample_tick = HAL_GetTick();
  device->data_ready = 1U;
  device->initialized = 1U;
  device->error = "none";
  return HAL_OK;
}

HAL_StatusTypeDef ADS1256_Init(ADS1256_HandleTypeDef *device)
{
  device->initialized = 0U;
  device->has_sample = 0U;
  device->chip_id = 0xFFU;
  device->data_ready = 0U;
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  uint32_t counter_start = DWT->CYCCNT;
  for (uint32_t index = 0U; index < 32U; ++index)
  {
    __NOP();
  }
  if (DWT->CYCCNT == counter_start)
  {
    return ADS1256_Fail(device, HAL_ERROR, "microsecond timer unavailable");
  }
  HAL_GPIO_WritePin(ADS_CS_GPIO_Port, ADS_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(ADS_SYNC_GPIO_Port, ADS_SYNC_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(ADS_RESET_GPIO_Port, ADS_RESET_Pin, GPIO_PIN_RESET);
  HAL_Delay(2U);
  HAL_GPIO_WritePin(ADS_RESET_GPIO_Port, ADS_RESET_Pin, GPIO_PIN_SET);
  HAL_Delay(5U);
  if (ADS1256_WaitReady() != HAL_OK)
  {
    return ADS1256_Fail(device, HAL_TIMEOUT, "DRDY timeout; check power/clock/PB0");
  }
  if (ADS1256_Command(device, 0x0FU) != HAL_OK)
  {
    return ADS1256_Fail(device, HAL_ERROR, "SDATAC failed; check SPI3");
  }
  ADS1256_DelayUs(8U);
  return ADS1256_Configure(device, device->channel, device->gain, device->rate);
}

HAL_StatusTypeDef ADS1256_Poll(ADS1256_HandleTypeDef *device)
{
  if (!device->initialized)
  {
    return HAL_ERROR;
  }
  if (!device->data_ready &&
      HAL_GPIO_ReadPin(ADS_DRDY_GPIO_Port, ADS_DRDY_Pin) != GPIO_PIN_RESET)
  {
    return HAL_BUSY;
  }
  device->data_ready = 0U;
  if (HAL_GPIO_ReadPin(ADS_DRDY_GPIO_Port, ADS_DRDY_Pin) != GPIO_PIN_RESET)
  {
    return HAL_BUSY;
  }
  uint8_t command = 0x01U;
  uint8_t dummy[] = {0xFFU, 0xFFU, 0xFFU};
  uint8_t received[3];
  ADS1256_Select();
  HAL_StatusTypeDef result = HAL_SPI_Transmit(device->spi, &command, 1U,
                                             ADS1256_SPI_TIMEOUT_MS);
  if (result == HAL_OK)
  {
    ADS1256_DelayUs(8U);
    result = HAL_SPI_TransmitReceive(device->spi, dummy, received, sizeof(received),
                                    ADS1256_SPI_TIMEOUT_MS);
  }
  ADS1256_Release();
  if (result != HAL_OK)
  {
    return ADS1256_Fail(device, result, "SPI sample read failed; send ADC INIT");
  }
  uint32_t packed = ((uint32_t)received[0] << 16U) |
                    ((uint32_t)received[1] << 8U) | received[2];
  device->raw = (packed & 0x800000U) ? (int32_t)packed - 16777216 : (int32_t)packed;
  device->has_sample = 1U;
  ADS1256_SetReference(device, device->reference_uv);
  device->sample_tick = HAL_GetTick();
  ++device->samples;
  return HAL_OK;
}

void ADS1256_DataReady(ADS1256_HandleTypeDef *device)
{
  device->data_ready = 1U;
  ++device->drdy_edges;
}
