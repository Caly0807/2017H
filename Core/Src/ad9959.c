#include "ad9959.h"
#include "dds_config.h"

#define AD9959_CSR 0x00U
#define AD9959_FR1 0x01U
#define AD9959_FR2 0x02U
#define AD9959_CFR 0x03U
#define AD9959_CFTW0 0x04U
#define AD9959_CPOW0 0x05U
#define AD9959_ACR 0x06U
#define AD9959_CFR_SINE 0x000301UL
#define AD9959_CFR_DISABLED 0x0003C1UL
#define AD9959_AMPLITUDE_ENABLE 0x001000UL

static void AD9959_Update(void)
{
  HAL_GPIO_WritePin(DDS_UPDATE_PORT, DDS_UPDATE_PIN, GPIO_PIN_SET);
  HAL_Delay(1);
  HAL_GPIO_WritePin(DDS_UPDATE_PORT, DDS_UPDATE_PIN, GPIO_PIN_RESET);
  HAL_Delay(1);
}

static HAL_StatusTypeDef AD9959_Write(AD9959_HandleTypeDef *device,
                                     uint8_t address, uint32_t value,
                                     uint32_t length)
{
  uint8_t packet[5];
  packet[0] = address;
  for (uint32_t index = 0; index < length; ++index)
  {
    packet[index + 1U] = (uint8_t)(value >> (8U * (length - index - 1U)));
  }
  HAL_GPIO_WritePin(DDS_CS_PORT, DDS_CS_PIN, GPIO_PIN_RESET);
  HAL_StatusTypeDef result = HAL_SPI_Transmit(device->spi, packet,
                                             (uint16_t)(length + 1U), 100);
  HAL_GPIO_WritePin(DDS_CS_PORT, DDS_CS_PIN, GPIO_PIN_SET);
  return result;
}

HAL_StatusTypeDef AD9959_ValidateClock(uint32_t reference_hz, uint32_t multiplier,
                                     uint32_t *system_hz)
{
  if (system_hz == NULL || reference_hz < 1000000UL ||
      reference_hz > 500000000UL ||
      (multiplier != 1U && (multiplier < 4U || multiplier > 20U)))
  {
    return HAL_ERROR;
  }
  uint64_t clock = (uint64_t)reference_hz * multiplier;
  if (clock > 500000000ULL)
  {
    return HAL_ERROR;
  }
  if (multiplier != 1U &&
      (reference_hz < 10000000UL || reference_hz > 125000000UL ||
       !((clock >= 100000000ULL && clock <= 160000000ULL) ||
         (clock >= 255000000ULL && clock <= 500000000ULL))))
  {
    return HAL_ERROR;
  }
  *system_hz = (uint32_t)clock;
  return HAL_OK;
}

HAL_StatusTypeDef AD9959_Init(AD9959_HandleTypeDef *device, SPI_HandleTypeDef *spi,
                            uint32_t reference_hz, uint32_t multiplier)
{
  uint32_t system_hz;
  if (device == NULL || spi == NULL ||
      AD9959_ValidateClock(reference_hz, multiplier, &system_hz) != HAL_OK)
  {
    return HAL_ERROR;
  }
  device->spi = spi;
  device->system_clock_hz = 0;
  HAL_GPIO_WritePin(DDS_CS_PORT, DDS_CS_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(DDS_UPDATE_PORT, DDS_UPDATE_PIN, GPIO_PIN_RESET);
  HAL_Delay(500);
  if (DDS_RESET_PORT != NULL)
  {
    GPIO_InitTypeDef reset_gpio = {0};
    reset_gpio.Pin = DDS_RESET_PIN;
    reset_gpio.Mode = GPIO_MODE_OUTPUT_PP;
    reset_gpio.Pull = GPIO_NOPULL;
    reset_gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_WritePin(DDS_RESET_PORT, DDS_RESET_PIN, GPIO_PIN_RESET);
    HAL_GPIO_Init(DDS_RESET_PORT, &reset_gpio);
    HAL_Delay(1);
    HAL_GPIO_WritePin(DDS_RESET_PORT, DDS_RESET_PIN, GPIO_PIN_SET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(DDS_RESET_PORT, DDS_RESET_PIN, GPIO_PIN_RESET);
    HAL_Delay(1);
  }
  if (AD9959_Write(device, AD9959_CSR, 0xF0, 1) != HAL_OK ||
      AD9959_Write(device, AD9959_CFR, AD9959_CFR_DISABLED, 3) != HAL_OK ||
      AD9959_Write(device, AD9959_ACR, AD9959_AMPLITUDE_ENABLE, 3) != HAL_OK ||
      AD9959_Write(device, AD9959_FR2, 0, 2) != HAL_OK)
  {
    return HAL_ERROR;
  }
  AD9959_Update();
  uint32_t fr1 = multiplier == 1U ? 0U : multiplier << 18;
  if (multiplier != 1U && system_hz >= 255000000UL)
  {
    fr1 |= 1UL << 23;
  }
  if (AD9959_Write(device, AD9959_FR1, fr1, 3) != HAL_OK)
  {
    return HAL_ERROR;
  }
  AD9959_Update();
  HAL_Delay(5);
  device->system_clock_hz = system_hz;
  return HAL_OK;
}

HAL_StatusTypeDef AD9959_SetTone(AD9959_HandleTypeDef *device, uint32_t channel,
                               uint32_t frequency_hz, uint32_t amplitude,
                               uint32_t phase_mdeg, uint8_t enabled)
{
  if (device == NULL || device->spi == NULL || device->system_clock_hz == 0U ||
      channel > 3U || frequency_hz == 0U ||
      frequency_hz > (device->system_clock_hz - 1U) / 2U ||
      amplitude > 1023U || phase_mdeg >= 360000UL)
  {
    return HAL_ERROR;
  }
  uint32_t tuning_word = (uint32_t)((((uint64_t)frequency_hz << 32) +
                                    device->system_clock_hz / 2U) /
                                   device->system_clock_hz);
  uint32_t phase_word = (uint32_t)(((uint64_t)phase_mdeg * 16384U + 180000U) /
                                   360000U) & 0x3FFFU;
  if (AD9959_Write(device, AD9959_CSR, 1UL << (channel + 4U), 1) != HAL_OK ||
      AD9959_Write(device, AD9959_CFR,
                   enabled ? AD9959_CFR_SINE : AD9959_CFR_DISABLED, 3) != HAL_OK ||
      AD9959_Write(device, AD9959_CFTW0, tuning_word, 4) != HAL_OK ||
      AD9959_Write(device, AD9959_CPOW0, phase_word, 2) != HAL_OK ||
      AD9959_Write(device, AD9959_ACR,
                   AD9959_AMPLITUDE_ENABLE | amplitude, 3) != HAL_OK)
  {
    return HAL_ERROR;
  }
  AD9959_Update();
  return HAL_OK;
}
