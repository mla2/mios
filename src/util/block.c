#include <mios/block.h>

#include <stdlib.h>

typedef struct {
  block_iface_t iface;
  block_iface_t *parent;
  size_t offset;
} partition_t;

static error_t
partition_erase(struct block_iface *bi, size_t block)
{
  partition_t *p = (partition_t *)bi;
  if (block >= p->iface.num_blocks)
   return ERR_NOSPC;
  return p->parent->erase(p->parent, block + p->offset);
}

static error_t
partition_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  if (block >= p->iface.num_blocks)
    return ERR_NOSPC;

  if (offset + length > p->iface.block_size)
    return ERR_INVALID_LENGTH;

  return p->parent->write(p->parent, block + p->offset, offset, data, length);
}

static error_t
partition_read(struct block_iface *bi, size_t block,
               size_t offset, void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  return p->parent->read(p->parent, block + p->offset, offset, data, length);
}

static error_t
partition_ctrl(struct block_iface *bi, block_ctrl_op_t op)
{
  partition_t *p = (partition_t *)bi;
  return p->parent->ctrl(p->parent, op);
}


static error_t
partition_lock_erase(struct block_iface *bi, size_t block)
{
  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_erase(bi, block);
  p->parent->ctrl(p->parent, BLOCK_UNLOCK);
  return err;
}

static error_t
partition_lock_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_write(bi, block, offset, data, length);
  p->parent->ctrl(p->parent, BLOCK_UNLOCK);
  return err;
}

static error_t
partition_lock_read(struct block_iface *bi, size_t block,
                    size_t offset, void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_read(bi, block, offset, data, length);
  p->parent->ctrl(p->parent, BLOCK_UNLOCK);
  return err;
}

static error_t
partition_lock_ctrl(struct block_iface *bi, block_ctrl_op_t op)
{
  if(op == BLOCK_LOCK || op == BLOCK_UNLOCK)
    return 0;

  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_ctrl(bi, op);
  p->parent->ctrl(p->parent, BLOCK_UNLOCK);
  return err;
}

block_iface_t *
block_create_partition(block_iface_t *parent,
                       size_t block_offset,
                       size_t num_blocks,
                       int flags)
{
  partition_t *p = malloc(sizeof(partition_t));

  p->iface.num_blocks = num_blocks;
  p->iface.block_size = parent->block_size;

  if(flags & BLOCK_PARTITION_AUTOLOCK) {
    p->iface.erase = partition_lock_erase;
    p->iface.write = partition_lock_write;
    p->iface.read = partition_lock_read;
    p->iface.ctrl = partition_lock_ctrl;
  } else {
    p->iface.erase = partition_erase;
    p->iface.write = partition_write;
    p->iface.read = partition_read;
    p->iface.ctrl = partition_ctrl;
  }

  p->parent = parent;
  p->offset = block_offset;
  return &p->iface;
}
