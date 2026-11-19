#ifndef ADC_CONSOLE_H
#define ADC_CONSOLE_H

#include "main.h"

void ADC_Console_Init(SPI_HandleTypeDef *spi, UART_HandleTypeDef *uart);
void ADC_Console_Process(void);
uint8_t ADC_Console_Command(char *const *tokens, uint32_t count);
void ADC_Console_Help(void);

#endif
