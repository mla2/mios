#pragma once

#include "stm32h7.h"

#define STM32H7_RCC_BASE 0x58024400

#define STM32H7_CLK_AHB4  0xe0
#define STM32H7_CLK_APB1L 0xe8
#define STM32H7_CLK_APB4  0xf4

#define CLK_ID(reg, bit) (((reg) << 8) | (bit))

#define CLK_SYSCFG CLK_ID(STM32H7_CLK_APB4, 1)

#define CLK_GPIOA CLK_ID(STM32H7_CLK_AHB4, 0)
#define CLK_GPIOB CLK_ID(STM32H7_CLK_AHB4, 1)
#define CLK_GPIOC CLK_ID(STM32H7_CLK_AHB4, 2)
#define CLK_GPIOD CLK_ID(STM32H7_CLK_AHB4, 3)
#define CLK_GPIOE CLK_ID(STM32H7_CLK_AHB4, 4)
#define CLK_GPIOF CLK_ID(STM32H7_CLK_AHB4, 5)
#define CLK_GPIOG CLK_ID(STM32H7_CLK_AHB4, 6)
#define CLK_GPIOH CLK_ID(STM32H7_CLK_AHB4, 7)
#define CLK_GPIOI CLK_ID(STM32H7_CLK_AHB4, 8)
#define CLK_GPIOJ CLK_ID(STM32H7_CLK_AHB4, 9)
#define CLK_GPIOK CLK_ID(STM32H7_CLK_AHB4, 10)

#define CLK_USART2 CLK_ID(STM32H7_CLK_APB1L, 17)
#define CLK_USART3 CLK_ID(STM32H7_CLK_APB1L, 18)
#define CLK_USART4 CLK_ID(STM32H7_CLK_APB1L, 19)
#define CLK_USART5 CLK_ID(STM32H7_CLK_APB1L, 20)

static inline void
clk_enable(uint16_t id)
{
  reg_set_bit(STM32H7_RCC_BASE + (id >> 8), id & 0xff);
}

unsigned int clk_get_freq(uint16_t id);

void stm32h7_init_pll(void);

