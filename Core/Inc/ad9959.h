#ifndef AD9959_H
#define AD9959_H

#include "main.h"

typedef struct
{
  SPI_HandleTypeDef *spi;
  uint32_t system_clock_hz;
} AD9959_HandleTypeDef;

HAL_StatusTypeDef AD9959_ValidateClock(uint32_t reference_hz, uint32_t multiplier,
                                     uint32_t *system_hz);
HAL_StatusTypeDef AD9959_Init(AD9959_HandleTypeDef *device, SPI_HandleTypeDef *spi,
                            uint32_t reference_hz, uint32_t multiplier);
HAL_StatusTypeDef AD9959_SetTone(AD9959_HandleTypeDef *device, uint32_t channel,
                               uint32_t frequency_hz, uint32_t amplitude,
                               uint32_t phase_mdeg, uint8_t enabled);

#endif
