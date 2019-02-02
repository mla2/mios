#include <stdint.h>

#include "mios.h"
#include "irq.h"
#include "platform.h"

void
irq(void)
{
  panic("Unexpected IRQ");
}

#include "irq_alias.h"


static volatile unsigned int * const NVIC_ISER = (unsigned int *)0xe000e100;
static volatile unsigned int * const NVIC_ICER = (unsigned int *)0xe000e180;
static volatile uint8_t * const NVIC_IPR  = (uint8_t *)0xe000e400;

void
irq_enable(int irq, int level)
{
  NVIC_IPR[irq] = IRQ_LEVEL_TO_PRI(level);
  NVIC_ISER[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}

void
irq_disable(int irq)
{
  NVIC_ICER[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}


static volatile unsigned int * const SYST_SHPR2 = (unsigned int *)0xe000ed1c;
static volatile unsigned int * const SYST_SHPR3 = (unsigned int *)0xe000ed20;

void
irq_init(void)
{
  *SYST_SHPR2 =
    (IRQ_LEVEL_TO_PRI(IRQ_LEVEL_SVC) << 24);

  *SYST_SHPR3 =
    (IRQ_LEVEL_TO_PRI(IRQ_LEVEL_CLOCK) << 24) |
    (IRQ_LEVEL_TO_PRI(IRQ_LEVEL_SWITCH) << 16);

  //  platform_console_init();

  asm volatile ("cpsie i\n\t"
                "isb\n\t");
}
