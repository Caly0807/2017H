#include "stm32h743xx.h"
#include "scope_replay_table.h"

enum {
    cycles_per_ms = 64000,
    point_count = 40,
    retrace_ms = 10,
    blank_code = 192
};

typedef struct {
    uint32_t signature;
    uint32_t status;
    uint32_t frames;
    uint32_t first_code;
    uint32_t last_code;
    uint32_t blank_code;
    uint32_t active_cycles;
    uint32_t frame_cycles;
} ReplayStatus;

static volatile ReplayStatus *const report = (volatile ReplayStatus *)0x24001000UL;

static void wait_until(uint32_t start, uint32_t cycles)
{
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
}

__attribute__((noinline, used)) void frame_ready(void)
{
    __asm volatile ("nop" ::: "memory");
}

static __attribute__((noreturn)) void stop(uint32_t status)
{
    report->status = status;
    frame_ready();
    while (1) {
        __NOP();
    }
}

__attribute__((section(".entry"), noreturn, used)) void scope_replay(void)
{
    __disable_irq();
    report->signature = 0x53435052;
    report->status = 0;
    report->frames = 0;
    if ((RCC->AHB4ENR & RCC_AHB4ENR_GPIOAEN) == 0 ||
        (GPIOA->MODER & GPIO_MODER_MODE4_Msk) != GPIO_MODER_MODE4_Msk) {
        stop(1);
    }

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *(volatile uint32_t *)0xE0001FB0UL = 0xC5ACCE55UL;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint32_t cycle_probe = DWT->CYCCNT;
    for (unsigned attempt = 0; attempt < 24; ++attempt) {
        __NOP();
    }
    if (DWT->CYCCNT == cycle_probe) {
        stop(2);
    }

    RCC->APB1LENR |= RCC_APB1LENR_DAC12EN;
    (void)RCC->APB1LENR;
    if ((DAC1->CR & DAC_CR_EN1) != 0) {
        stop(3);
    }
    GPIOA->PUPDR &= ~GPIO_PUPDR_PUPD4_Msk;
    DAC1->MCR &= ~DAC_MCR_MODE1_Msk;
    DAC1->CR &= ~DAC_CR_TEN1;
    DAC1->DHR12R1 = blank_code;
    DAC1->CR |= DAC_CR_EN1;
    wait_until(DWT->CYCCNT, cycles_per_ms * retrace_ms);

    while (1) {
        uint32_t frame_start = DWT->CYCCNT;
        for (unsigned point = 0; point < point_count; ++point) {
            DAC1->DHR12R1 = replay_codes[point];
            if (point == 0 || point == point_count - 1) {
                wait_until(frame_start, cycles_per_ms * point + 3200);
                if (point == 0) {
                    report->first_code = DAC1->DOR1;
                } else {
                    report->last_code = DAC1->DOR1;
                }
            }
            wait_until(frame_start, cycles_per_ms * (point + 1));
        }
        DAC1->DHR12R1 = blank_code;
        report->active_cycles = DWT->CYCCNT - frame_start;
        wait_until(frame_start, cycles_per_ms * (point_count + 1));
        report->blank_code = DAC1->DOR1;
        wait_until(frame_start, cycles_per_ms * (point_count + retrace_ms));
        report->frame_cycles = DWT->CYCCNT - frame_start;
        if (report->first_code != replay_codes[0] ||
            report->last_code != replay_codes[point_count - 1] || report->blank_code != blank_code) {
            stop(4);
        }
        ++report->frames;
        report->status = 0x600d0001;
        frame_ready();
    }
}
