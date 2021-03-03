#pragma once

#include <sys/queue.h>
#include <stddef.h>
#include <stdint.h>

#define PBUF_DATA_SIZE 512



STAILQ_HEAD(pbuf_queue, pbuf);

#define PBUF_SOP 0x1
#define PBUF_EOP 0x2
#define PBUF_BCAST 0x4
#define PBUF_MCAST 0x8

typedef struct pbuf {

  STAILQ_ENTRY(pbuf) pb_link;
#define pb_next pb_link.stqe_next

  uint16_t pb_flags;
  uint16_t pb_pktlen;
  uint16_t pb_offset;
  uint16_t pb_buflen;

  void *pb_data;

} pbuf_t;


static inline void *pbuf_data(pbuf_t *pb, size_t offset) {
  return pb->pb_data + pb->pb_offset + offset;
}

pbuf_t *pbuf_drop(pbuf_t *pb, size_t bytes);

pbuf_t *pbuf_prepend(pbuf_t *pb, size_t bytes);

pbuf_t *pbuf_pullup(pbuf_t *pb, size_t bytes);

void pbuf_free(pbuf_t *pb);

pbuf_t *pbuf_make(int offset);

void *pbuf_append(pbuf_t *pb, size_t bytes);

pbuf_t *pbuf_splice(struct pbuf_queue *pq);

void pbuf_print(const char *prefix, pbuf_t *pb);

// =========================================================
// All functions below here assume irq_forbid(IRQ_LEVEL_NET)
// =========================================================

void pbuf_data_add(void *start, void *end);

void *pbuf_data_get(int wait);

void pbuf_data_put(void *ptr);

void pbuf_alloc(size_t count);

pbuf_t *pbuf_get(int wait);

void pbuf_put(pbuf_t *pb);

void pbuf_free_irq_blocked(pbuf_t *pb);