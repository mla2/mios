#include <stdarg.h>

#include <mios/cli.h>
#include <mios/error.h>
#include <stdio.h>
#include <malloc.h>

#include "pcs/pcs.h"
#include "pcs_shell.h"

typedef struct {
  stream_t s;
  pcs_t *pcs;

} pcs_shell_stream_t;


static int
pcs_shell_read(struct stream *s, void *buf, size_t size, int wait)
{
  pcs_shell_stream_t *pss = (pcs_shell_stream_t *)s;

  return pcs_read(pss->pcs, buf, size,
                  wait == STREAM_READ_WAIT_ALL ? size : wait);
}


static void
pcs_shell_write(struct stream *s, const void *buf, size_t size)
{
  pcs_shell_stream_t *pss = (pcs_shell_stream_t *)s;

  if(buf == NULL)
    pcs_flush(pss->pcs);
  else if(size)
    pcs_send(pss->pcs, buf, size);
}


__attribute__((noreturn))
static void *
pcs_shell(void *arg)
{
  pcs_shell_stream_t pss;
  pss.s.read = pcs_shell_read;
  pss.s.write = pcs_shell_write;
  pss.pcs = arg;

  cli_on_stream(&pss.s, '>');
  pcs_close(pss.pcs);
  task_exit(NULL);
}


int
pcs_shell_create(pcs_t *pcs)
{
  return task_create_shell(pcs_shell, pcs, "remotecli");
}


void *
pcs_malloc(size_t size)
{
  return xalloc(size, 0, MEM_MAY_FAIL);
}
