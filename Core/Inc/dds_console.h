#ifndef DDS_CONSOLE_H
#define DDS_CONSOLE_H

#include "main.h"

void DDS_Console_Init(SPI_HandleTypeDef *spi, UART_HandleTypeDef *uart);
void DDS_Console_Process(void);

#endif
