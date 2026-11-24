#ifndef ADS1256_H
#define ADS1256_H

#include "main.h"

typedef struct
{
  SPI_HandleTypeDef *spi;
  uint32_t reference_uv;
  uint32_t rate;
  uint32_t samples;
  uint32_t sample_tick;
  uint32_t drdy_events;
  int32_t raw;
  int32_t voltage_uv;
  const char *error;
  uint8_t channel;
  uint8_t gain;
  uint8_t chip_id;
  uint8_t initialized;
  uint8_t has_sample;
  uint8_t wait_drdy_high;
} ADS1256_HandleTypeDef;

HAL_StatusTypeDef ADS1256_Init(ADS1256_HandleTypeDef *device);
HAL_StatusTypeDef ADS1256_Configure(ADS1256_HandleTypeDef *device,
                                  uint8_t channel, uint8_t gain, uint32_t rate);
HAL_StatusTypeDef ADS1256_Poll(ADS1256_HandleTypeDef *device);
void ADS1256_SetReference(ADS1256_HandleTypeDef *device, uint32_t reference_uv);
uint8_t ADS1256_ValidSettings(uint8_t channel, uint8_t gain, uint32_t rate);

#endif
