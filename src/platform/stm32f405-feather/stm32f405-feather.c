#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <io.h>

#include "mios.h"
#include "irq.h"
#include "task.h"

#include "stm32f4.h"
#include "stm32f4_i2c.h"

#include "uart.h"

#define NEOPIX_GPIO GPIO_PC(0)
#define PANIC_GPIO  GPIO_PA(4)
#define BLINK_GPIO  GPIO_PC(1) // Red led close to USB connection

static uart_t console;

void
irq_71(void)
{
  uart_irq(&console);
}


static void __attribute__((constructor(110)))
board_init_console(void)
{
  reg_set_bit(RCC_APB2ENR, 5); // CLK ENABLE: USART6

  // Configure PA2 for USART6 TX (Alternative Function 8)
  gpio_conf_af(GPIO_PC(6), 8,
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  gpio_conf_af(GPIO_PC(7), 8,
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  uart_init(&console, 0x40011400, 115200 / 2);

  irq_enable(71, IRQ_LEVEL_CONSOLE);

  init_printf(&console, uart_putc);
  init_getchar(&console, uart_getc);
}


static void __attribute__((constructor(101)))
board_setup_clocks(void)
{
  reg_wr(FLASH_ACR, 0x75); // D-CACHE I-CACHE PREFETCH, 5 wait states

  reg_wr(RCC_CFGR,
         (0x7 << 27) | // MCO2PRE /5
         (0x4 << 13) | // APB2 (High speed) prescaler = 2
         (0x5 << 10)); // APB1 (Low speed)  prescaler = 4

  reg_set_bit(RCC_CR, 16); // HSEON

  while(!(reg_rd(RCC_CR) & (1 << 17))) {} // Wait for external oscillator

  reg_wr(RCC_PLLCFGR,
         (1 << 22)
         | (6 << 0)         // input division (12MHz external xtal)
         | (168 << 6)       // PLL multiplication
         | (0 << 16)        // PLL sys clock division (0 == /2) */
         | (7 << 24));      // PLL usb clock division =48MHz */

  reg_set_bit(RCC_CR, 24);

  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 2, 2); // Use PLL as system clock

  while((reg_rd(RCC_CFGR) & 0xc) != 0x8) {}


  reg_set_bit(RCC_APB2ENR, 14); // CLK ENABLE: SYSCFG

  reg_set(RCC_AHB1ENR, 0x07);    // CLK ENABLE: GPIOA,B,C

  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);

  gpio_set_output(BLINK_GPIO, 1); // Red LED
}



#include "cpu.h"

static inline void __attribute__((always_inline))
cyclewait(uint32_t ref, uint32_t t)
{
  while(cpu_cycle_counter() - ref < t) {
  }
}



static void
neopix_init(void)
{
  gpio_set_output(NEOPIX_GPIO, 0);
  gpio_conf_output(NEOPIX_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
}

void
neopix(uint8_t r, uint8_t g, uint8_t b)
{
  int word = (g << 24) | (r << 16) | (b << 8);
  const gpio_t pin = NEOPIX_GPIO;

  int q = irq_forbid(IRQ_LEVEL_ALL);
  uint32_t ref = cpu_cycle_counter();
  uint32_t t0 = 0;

  for(int j = 0; j < 24; j++, word <<= 1) {

    cyclewait(ref, t0);
    gpio_set_output(pin, 1);

    int o = word < 0 ? 134 : 67;
    cyclewait(ref, t0 + o);
    gpio_set_output(pin, 0);
    t0 += 201;
  }
  irq_permit(q);
}

#include "cli.h"
#include <stdlib.h>

static int
cmd_neopix(cli_t *c, int argc, char **argv)
{
  if(argc < 4)
    return -1;
  neopix(atoi(argv[1]),atoi(argv[2]),atoi(argv[3]));
  return 0;
}

CLI_CMD_DEF("neopix", cmd_neopix);


static void *
blinker(void *arg)
{
  neopix_init();
  usleep(20);
  neopix(0,0,10);

  while(1) {
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 0); // Red LED
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 1); // Red LED
  }
  return NULL;
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  gpio_set_output(PANIC_GPIO, 0);
  gpio_conf_output(PANIC_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  task_create(blinker, NULL, 512, "blinker", 0, 0);


}



void
platform_panic(void)
{
  gpio_set_output(PANIC_GPIO, 1);
  neopix(10,0,10);
}
