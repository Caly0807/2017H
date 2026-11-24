#include "drdy_diag.h"
#include "main.h"

enum
{
  DRDY_DIAG_RUNNING = 1U,
  DRDY_DIAG_DONE = 2U,
  DRDY_DIAG_CLOCK_UNSUPPORTED = 3U,
  DRDY_DIAG_CAPTURE_LIMIT = 4U,
  DRDY_DIAG_TICK_HZ = 100000U,
  DRDY_DIAG_WINDOW_TICKS = DRDY_DIAG_TICK_HZ / 2U,
  DRDY_DIAG_MAX_CAPTURES = 32768U
};

typedef struct
{
  uint32_t magic;
  uint32_t status;
  uint32_t tick_hz;
  uint32_t elapsed_ticks;
  uint32_t captures;
  uint32_t overcapture_events;
  uint32_t min_ticks;
  uint32_t max_ticks;
  uint32_t short_gaps;
  uint32_t nominal_gaps;
} DRDY_DiagResult;

_Static_assert(sizeof(DRDY_DiagResult) == 40U, "SWD reader ABI");
const uint32_t g_drdy_diag_tag = 0x44524431U;
volatile DRDY_DiagResult g_drdy_diag;
static uint32_t last_capture;

void DRDY_Diag_Start(void)
{
  g_drdy_diag.magic = *(const volatile uint32_t *)&g_drdy_diag_tag;
  uint32_t timer_clock = HAL_RCC_GetHCLKFreq();
  if (HAL_RCC_GetPCLK1Freq() * 2U != timer_clock ||
      timer_clock < DRDY_DIAG_TICK_HZ ||
      timer_clock % DRDY_DIAG_TICK_HZ != 0U ||
      timer_clock / DRDY_DIAG_TICK_HZ > 65536U)
  {
    g_drdy_diag.status = DRDY_DIAG_CLOCK_UNSUPPORTED;
    return;
  }

  HAL_NVIC_DisableIRQ(ADS_DRDY_EXTI_IRQn);
  HAL_GPIO_DeInit(ADS_DRDY_GPIO_Port, ADS_DRDY_Pin);
  GPIO_InitTypeDef input = {0};
  input.Pin = ADS_DRDY_Pin;
  input.Mode = GPIO_MODE_AF_PP;
  input.Pull = GPIO_PULLUP;
  input.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(ADS_DRDY_GPIO_Port, &input);

  __HAL_RCC_TIM3_CLK_ENABLE();
  TIM3->CR1 = TIM_CR1_OPM | TIM_CR1_URS;
  TIM3->PSC = timer_clock / DRDY_DIAG_TICK_HZ - 1U;
  TIM3->ARR = DRDY_DIAG_WINDOW_TICKS - 1U;
  TIM3->CCMR2 = TIM_CCMR2_CC3S_0;
  TIM3->EGR = TIM_EGR_UG;
  TIM3->SR = 0U;
  g_drdy_diag.tick_hz = DRDY_DIAG_TICK_HZ;
  g_drdy_diag.status = DRDY_DIAG_RUNNING;
  TIM3->CR1 |= TIM_CR1_CEN;
  TIM3->CCER = TIM_CCER_CC3E | TIM_CCER_CC3P;
  TIM3->DIER = TIM_DIER_CC3IE | TIM_DIER_UIE;
  HAL_NVIC_SetPriority(TIM3_IRQn, 6U, 0U);
  HAL_NVIC_EnableIRQ(TIM3_IRQn);
}

static void DRDY_Diag_Finish(uint32_t status, uint32_t elapsed_ticks)
{
  TIM3->DIER = 0U;
  TIM3->CCER = 0U;
  TIM3->CR1 &= ~TIM_CR1_CEN;
  TIM3->SR = 0U;
  g_drdy_diag.elapsed_ticks = elapsed_ticks;
  __DMB();
  g_drdy_diag.status = status;
}

void TIM3_IRQHandler(void)
{
  uint32_t flags = TIM3->SR;
  if ((flags & TIM_SR_CC3OF) != 0U)
  {
    TIM3->SR = ~TIM_SR_CC3OF;
    ++g_drdy_diag.overcapture_events;
  }
  if ((flags & TIM_SR_UIF) != 0U)
  {
    DRDY_Diag_Finish(DRDY_DIAG_DONE, DRDY_DIAG_WINDOW_TICKS);
    return;
  }
  if ((flags & TIM_SR_CC3IF) == 0U)
  {
    return;
  }

  uint32_t capture = TIM3->CCR3;
  uint32_t count = g_drdy_diag.captures;
  if (count != 0U && capture < last_capture)
  {
    return;
  }
  if (count != 0U)
  {
    uint32_t gap = capture - last_capture;
    if (count == 1U || gap < g_drdy_diag.min_ticks)
    {
      g_drdy_diag.min_ticks = gap;
    }
    if (gap > g_drdy_diag.max_ticks)
    {
      g_drdy_diag.max_ticks = gap;
    }
    if (gap < DRDY_DIAG_TICK_HZ / 1000U)
    {
      ++g_drdy_diag.short_gaps;
    }
    if (gap >= DRDY_DIAG_TICK_HZ * 8U / 1000U && gap <= DRDY_DIAG_TICK_HZ * 12U / 1000U)
    {
      ++g_drdy_diag.nominal_gaps;
    }
  }
  last_capture = capture;
  g_drdy_diag.captures = count + 1U;
  if (count + 1U >= DRDY_DIAG_MAX_CAPTURES)
  {
    DRDY_Diag_Finish(DRDY_DIAG_CAPTURE_LIMIT, capture);
  }
}
