#include "stm32g4_reg.h"
#include "stm32g4_clk.h"

#include <mios/cli.h>
#include <mios/sys.h>


static uint32_t reset_reason;

static void  __attribute__((constructor(102)))
stm32g4_get_reset_reason(void)
{
  uint32_t rr = reg_rd(RCC_CSR);

  uint32_t n = 0;
  if(rr & (1 << 31)) {
    n |= RESET_REASON_LOW_POWER_RESET;
  } else if(rr & (1 << 30)) {
    n |= RESET_REASON_WATCHDOG;
  } else if(rr & (1 << 29)) {
    n |= RESET_REASON_WATCHDOG;
  } else if(rr & (1 << 28)) {
    n |= RESET_REASON_SW_RESET;
  } else if(rr & (1 << 27)) {
    n |= RESET_REASON_BROWNOUT;
  } else if(rr & (1 << 26)) {
    n |= RESET_REASON_EXT_RESET;
  }
  reg_wr(RCC_CSR, rr | (1 << 23));
  reset_reason = n;
}

reset_reason_t
sys_get_reset_reason(void)
{
  return reset_reason;
}


const struct serial_number
sys_get_serial_number(void)
{
  struct serial_number sn;
  sn.data = (const void *)0x1fff7590;
  sn.len = 12;
  return sn;
}
