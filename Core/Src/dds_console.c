#include "dds_console.h"
#include "adc_console.h"
#include "ad9959.h"
#include "dds_config.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DDS_RX_CAPACITY 1024U
#define DDS_LINE_CAPACITY 128U

typedef struct
{
  uint32_t frequency_hz;
  uint32_t amplitude;
  uint32_t phase_mdeg;
  uint32_t reference_hz;
  uint32_t multiplier;
  uint8_t enabled;
} DDS_Settings;

static AD9959_HandleTypeDef dds;
static SPI_HandleTypeDef *dds_spi;
static UART_HandleTypeDef *console_uart;
static DDS_Settings settings = {
  DDS_START_FREQUENCY_HZ, DDS_START_AMPLITUDE, DDS_START_PHASE_MDEG,
  DDS_REFERENCE_CLOCK_HZ, DDS_PLL_MULTIPLIER, 1
};
static uint8_t initialized;
static uint8_t rx_byte;
static uint8_t rx_buffer[DDS_RX_CAPACITY];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;
static volatile uint8_t rx_fault;
static char line[DDS_LINE_CAPACITY];
static uint32_t line_length;
static uint8_t discard_line;

static void Console_Print(const char *format, ...)
{
  char output[256];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(output, sizeof(output), format, arguments);
  va_end(arguments);
  if (length > 0)
  {
    uint16_t count = (uint16_t)((size_t)length < sizeof(output) ?
                               (size_t)length : sizeof(output) - 1U);
    HAL_UART_Transmit(console_uart, (uint8_t *)output, count, 200);
  }
}

static void Console_Help(void)
{
  Console_Print("\r\nSET <freq> [amp [phase]]  or  <freq> [amp [phase]]\r\n");
  Console_Print("FREQ <freq> | AMP <0..1023> | PHASE <0..359.999>\r\n");
  Console_Print("ON | OFF | STATUS | HELP | INIT\r\n");
  Console_Print("CLOCK <reference> <1|4..20>  (1 = PLL bypass)\r\n");
  Console_Print("Freq units: Hz, kHz, MHz, k, M; no suffix = Hz. No spaces in a value.\r\n");
  Console_Print("Example: SET 1MHz 512 0    End each command with CR or LF.\r\n");
  ADC_Console_Help();
}

static void Console_Status(void)
{
  Console_Print("%s CH%u %s FREQ=%luHz AMP=%lu/1023 PHASE=%lu.%03ludeg\r\n",
                initialized ? "OK" : "NOT_READY", (unsigned)DDS_OUTPUT_CHANNEL,
                settings.enabled ? "ON" : "OFF",
                (unsigned long)settings.frequency_hz,
                (unsigned long)settings.amplitude,
                (unsigned long)(settings.phase_mdeg / 1000U),
                (unsigned long)(settings.phase_mdeg % 1000U));
  Console_Print("REF=%luHz PLL=%lu SYSCLK=%luHz; write-only, no readback/lock check\r\n",
                (unsigned long)settings.reference_hz,
                (unsigned long)settings.multiplier,
                (unsigned long)dds.system_clock_hz);
}

static uint8_t Parse_Value(const char *text, uint32_t scale, uint8_t frequency,
                           uint32_t *value)
{
  uint64_t whole = 0;
  uint64_t fraction = 0;
  uint32_t fraction_scale = 1;
  uint32_t digits = 0;
  while (isdigit((unsigned char)*text))
  {
    whole = whole * 10U + (uint32_t)(*text++ - '0');
    if (whole > UINT32_MAX)
    {
      return 0;
    }
    ++digits;
  }
  if (*text == '.')
  {
    ++text;
    if (!isdigit((unsigned char)*text))
    {
      return 0;
    }
    while (isdigit((unsigned char)*text))
    {
      if (fraction_scale == 1000000U)
      {
        return 0;
      }
      fraction = fraction * 10U + (uint32_t)(*text++ - '0');
      fraction_scale *= 10U;
      ++digits;
    }
  }
  if (digits == 0U)
  {
    return 0;
  }
  if (frequency)
  {
    if (strcmp(text, "m") == 0 || strcmp(text, "mhz") == 0)
    {
      scale = 1000000U;
    }
    else if (strcmp(text, "k") == 0 || strcmp(text, "khz") == 0)
    {
      scale = 1000U;
    }
    else if (*text != '\0' && strcmp(text, "hz") != 0)
    {
      return 0;
    }
  }
  else if (*text != '\0')
  {
    return 0;
  }
  uint64_t result = whole * scale +
                    (fraction * scale + fraction_scale / 2U) / fraction_scale;
  if (result > UINT32_MAX)
  {
    return 0;
  }
  *value = (uint32_t)result;
  return 1;
}

static uint8_t Parse_Amplitude(const char *text, uint32_t *value)
{
  for (const char *cursor = text; *cursor != '\0'; ++cursor)
  {
    if (!isdigit((unsigned char)*cursor))
    {
      return 0;
    }
  }
  return Parse_Value(text, 1, 0, value) && *value <= 1023U;
}

static uint8_t Console_Apply(const DDS_Settings *candidate, uint8_t reset)
{
  uint32_t system_hz;
  if (AD9959_ValidateClock(candidate->reference_hz, candidate->multiplier,
                           &system_hz) != HAL_OK)
  {
    Console_Print("ERR clock: REF 1..500MHz; PLL 1 or 4..20; PLL SYSCLK 100..160 or 255..500MHz\r\n");
    return 0;
  }
  if (candidate->frequency_hz == 0U ||
      candidate->frequency_hz > (system_hz - 1U) / 2U ||
      candidate->amplitude > 1023U || candidate->phase_mdeg >= 360000UL)
  {
    Console_Print("ERR range: 1 <= freq < %luHz; amp 0..1023; phase 0..359.999\r\n",
                  (unsigned long)(system_hz / 2U));
    return 0;
  }
  if (!reset && !initialized)
  {
    Console_Print("ERR device not ready; send INIT\r\n");
    return 0;
  }
  if ((reset && AD9959_Init(&dds, dds_spi, candidate->reference_hz,
                             candidate->multiplier) != HAL_OK) ||
      AD9959_SetTone(&dds, DDS_OUTPUT_CHANNEL, candidate->frequency_hz,
                      candidate->amplitude, candidate->phase_mdeg,
                      candidate->enabled) != HAL_OK)
  {
    initialized = 0;
    if (DDS_RESET_PORT == NULL)
    {
      Console_Print("ERR SPI transfer; hardware state unconfirmed; hardware-reset AD9959, then send INIT\r\n");
    }
    else
    {
      Console_Print("ERR SPI transfer; hardware state unconfirmed; INIT will pulse RST and retry\r\n");
    }
    return 0;
  }
  settings = *candidate;
  initialized = 1;
  return 1;
}

static void Console_Command(void)
{
  char *tokens[6];
  uint32_t count = 0;
  for (char *cursor = line; *cursor != '\0'; ++cursor)
  {
    *cursor = (char)tolower((unsigned char)*cursor);
  }
  char *token = strtok(line, " \t");
  while (token != NULL && count < 6U)
  {
    tokens[count++] = token;
    token = strtok(NULL, " \t");
  }
  if (count == 0U)
  {
    return;
  }
  if (ADC_Console_Command(tokens, count))
  {
    return;
  }
  if (count == 1U && (strcmp(tokens[0], "help") == 0 || strcmp(tokens[0], "?") == 0))
  {
    Console_Help();
    return;
  }
  if (count == 1U && strcmp(tokens[0], "status") == 0)
  {
    Console_Status();
    return;
  }
  DDS_Settings candidate = settings;
  uint8_t reset = 0;
  uint32_t first = strcmp(tokens[0], "set") == 0 ? 1U : 0U;
  if (count == 1U && strcmp(tokens[0], "init") == 0)
  {
    reset = 1;
  }
  else if (count == 1U && strcmp(tokens[0], "on") == 0)
  {
    candidate.enabled = 1;
  }
  else if (count == 1U && strcmp(tokens[0], "off") == 0)
  {
    candidate.enabled = 0;
  }
  else if (count == 2U && strcmp(tokens[0], "freq") == 0)
  {
    if (!Parse_Value(tokens[1], 1, 1, &candidate.frequency_hz))
    {
      goto syntax_error;
    }
  }
  else if (count == 2U && strcmp(tokens[0], "amp") == 0)
  {
    if (!Parse_Amplitude(tokens[1], &candidate.amplitude))
    {
      goto syntax_error;
    }
  }
  else if (count == 2U && strcmp(tokens[0], "phase") == 0)
  {
    if (!Parse_Value(tokens[1], 1000, 0, &candidate.phase_mdeg))
    {
      goto syntax_error;
    }
  }
  else if (count == 3U && strcmp(tokens[0], "clock") == 0)
  {
    if (!Parse_Value(tokens[1], 1, 1, &candidate.reference_hz) ||
        !Parse_Amplitude(tokens[2], &candidate.multiplier))
    {
      goto syntax_error;
    }
    reset = 1;
  }
  else if (count > first && count <= first + 3U)
  {
    if (!Parse_Value(tokens[first], 1, 1, &candidate.frequency_hz) ||
        (count > first + 1U && !Parse_Amplitude(tokens[first + 1U], &candidate.amplitude)) ||
        (count > first + 2U && !Parse_Value(tokens[first + 2U], 1000, 0, &candidate.phase_mdeg)))
    {
      goto syntax_error;
    }
    candidate.enabled = 1;
  }
  else
  {
    goto syntax_error;
  }
  if (Console_Apply(&candidate, reset))
  {
    Console_Status();
  }
  return;

syntax_error:
  Console_Print("ERR syntax or value; send HELP. Example: SET 1MHz 512 0\r\n");
}

void DDS_Console_Init(SPI_HandleTypeDef *spi, UART_HandleTypeDef *uart)
{
  dds_spi = spi;
  console_uart = uart;
  if (HAL_UART_Receive_IT(console_uart, &rx_byte, 1) != HAL_OK)
  {
    Error_Handler();
  }
  Console_Print("\r\nAD9959 CH%u console | USART2 115200 8N1\r\n", (unsigned)DDS_OUTPUT_CHANNEL);
  Console_Print("Clock assumption: REF=%luHz x%lu; verify module clock and reset.\r\n",
                (unsigned long)settings.reference_hz, (unsigned long)settings.multiplier);
  Console_Apply(&settings, 1);
  Console_Status();
  Console_Print("Send HELP for commands.\r\n");
}

void DDS_Console_Process(void)
{
  while (rx_tail != rx_head || rx_fault)
  {
    uint32_t interrupt_state = __get_PRIMASK();
    __disable_irq();
    if (rx_fault)
    {
      rx_tail = rx_head;
      rx_fault = 0;
      __set_PRIMASK(interrupt_state);
      line_length = 0;
      discard_line = 1;
      Console_Print("ERR RX overflow/UART error; send a newline, then resend command\r\n");
      continue;
    }
    uint8_t character = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1U) % DDS_RX_CAPACITY;
    __set_PRIMASK(interrupt_state);
    if (character == '\r' || character == '\n')
    {
      if (!discard_line && line_length != 0U)
      {
        line[line_length] = '\0';
        Console_Command();
      }
      line_length = 0;
      discard_line = 0;
    }
    else if (discard_line)
    {
      continue;
    }
    else if (character == '\b' || character == 127U)
    {
      if (line_length != 0U)
      {
        --line_length;
      }
    }
    else if ((character >= 32U && character <= 126U) || character == '\t')
    {
      if (line_length + 1U < sizeof(line))
      {
        line[line_length++] = (char)character;
      }
      else
      {
        discard_line = 1;
        line_length = 0;
        Console_Print("ERR line too long; discarded through next newline\r\n");
      }
    }
    else
    {
      discard_line = 1;
      line_length = 0;
      Console_Print("ERR non-ASCII command; discarded through next newline\r\n");
    }
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
  if (uart == console_uart)
  {
    uint32_t next = (rx_head + 1U) % DDS_RX_CAPACITY;
    if (next == rx_tail)
    {
      rx_fault = 1;
    }
    else
    {
      rx_buffer[rx_head] = rx_byte;
      __DMB();
      rx_head = next;
    }
    if (HAL_UART_Receive_IT(uart, &rx_byte, 1) != HAL_OK)
    {
      rx_fault = 1;
    }
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
  if (uart == console_uart)
  {
    rx_fault = 1;
    if (uart->RxState == HAL_UART_STATE_READY)
    {
      HAL_UART_Receive_IT(uart, &rx_byte, 1);
    }
  }
}
