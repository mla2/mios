#include <mios/datetime.h>
#include <mios/dsig.h>
#include <mios/cli.h>
#include <mios/eventlog.h>

#include <unistd.h>
#include <stdlib.h>
#include <string.h>


wallclock_t wallclock;

uint64_t
datetime_get_utc_usec(void)
{
  return clock_get() + wallclock.utc_offset;
}

uint32_t
datetime_get_utc_sec(void)
{
  return datetime_get_utc_usec() / 1000000;
}

void
datetime_from_unixtime(uint32_t t, datetime_t *dt)
{
   uint32_t a;
   uint32_t b;
   uint32_t c;
   uint32_t d;
   uint32_t e;
   uint32_t f;

   dt->sec = t % 60;
   t /= 60;
   dt->min = t % 60;
   t /= 60;
   dt->hour = t % 24;
   t /= 24;

   a = (uint32_t) ((4 * t + 102032) / 146097 + 15);
   b = (uint32_t) (t + 2442113 + a - (a / 4));
   c = (20 * b - 2442) / 7305;
   d = b - 365 * c - (c / 4);
   e = d * 1000 / 30601;
   f = d - e * 30 - e * 601 / 1000;

   if(e <= 13) {
      c -= 4716;
      e -= 1;
   } else {
     c -= 4715;
     e -= 13;
   }

   dt->year = c;
   dt->mon = e;
   dt->mday = f;
}


void
datetime_adj(datetime_adj_hand_t which, int delta)
{
  static const uint8_t days_per_month[12] = {31,28,31,30,31,30,
                                             31,31,30,31,30,31};
  datetime_t dt;

  switch(which) {
  case DATETIME_YEAR:
    delta *= 31536000;
    break;
  case DATETIME_MON:
    datetime_from_unixtime(datetime_get_utc_sec(), &dt);
    delta *= 86400 * days_per_month[dt.mon - 1];
    break;
  case DATETIME_MDAY:
    delta *= 86400;
    break;
  case DATETIME_HOUR:
    delta *= 3600;
    break;
  case DATETIME_MIN:
    delta *= 60;
    break;
  case DATETIME_SEC:
    break;
  }

  wallclock.utc_offset += (delta * 1000000ULL);
}


void
datetime_set_utc_offset(int64_t offset, const char *source)
{
  if(!wallclock.utc_offset) {
    wallclock.source = source;
    wallclock.utc_offset = offset;
    evlog(LOG_INFO, "Clock updated via %s (uptime:%lld µs)",
          source, clock_get());
  }
}


static error_t
cmd_time(cli_t *cli, int argc, char **argv)
{
  datetime_t dt;

  int loop = argc > 1;

  while(1) {

    int64_t t = datetime_get_utc_usec();
    datetime_from_unixtime(t / 1000000, &dt);
    cli_printf(cli, "%04d-%02d-%02d %02d:%02d:%02d.%06d +%02d:%02d (%s)\n",
               dt.year,
               dt.mon,
               dt.mday,
               dt.hour,
               dt.min,
               dt.sec,
               (int)(t % 1000000),
               wallclock.tz_offset / 60,
               wallclock.tz_offset % 60,
               wallclock.source ?: "not-synchronized");
    if(!loop)
      break;
    usleep(25000);
  }
  return 0;
}

CLI_CMD_DEF("time", cmd_time);
