#pragma once

#include <stdint.h>
#include <sys/queue.h>

#define HZ 100

typedef struct timer {
  LIST_ENTRY(timer) t_link;
  void (*t_cb)(void *opaque);
  void *t_opaque;
  unsigned int t_countdown;
} timer_t;


void timer_arm(timer_t *t, unsigned int delta);

void timer_disarm(timer_t *t);

uint64_t clock_get(void);

