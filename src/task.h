#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

#include "timer.h"

TAILQ_HEAD(task_queue, task);

#define TASK_STATE_RUNNING  0
#define TASK_STATE_SLEEPING 1
#define TASK_STATE_ZOMBIE   2

typedef struct task {
  TAILQ_ENTRY(task) t_link;
  const char *t_name;
  uint8_t t_state;
  void *t_sp;
  uint8_t t_stack[0];
} task_t;

extern task_t *curtask;

typedef struct mutex {
  struct task_queue waiters;
  struct task *owner;
} mutex_t;

task_t *task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
                    const char *name);

void task_wakeup(struct task_queue *waitable, int all);

void task_sleep(struct task_queue *waitable, int ticks);

void mutex_init(mutex_t *m);

void mutex_lock(mutex_t *m);

void mutex_unlock(mutex_t *m);

