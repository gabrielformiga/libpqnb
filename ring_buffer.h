#ifndef PQNB_RING_BUFFER_H
#define PQNB_RING_BUFFER_H

#include "stddef.h"
#include "stdbool.h"

struct PQNB_ring_buffer;

struct PQNB_ring_buffer*
PQNB_ring_buffer_init(size_t capacity, size_t sz);

void
PQNB_ring_buffer_free(struct PQNB_ring_buffer *cb);

bool
PQNB_ring_buffer_empty(struct PQNB_ring_buffer *cb);

bool
PQNB_ring_buffer_not_empty(struct PQNB_ring_buffer *cb);

int
PQNB_ring_buffer_push(struct PQNB_ring_buffer *cb, const void *item);

void*
PQNB_ring_buffer_pop(struct PQNB_ring_buffer *cb);

#endif /* ~PQNB_RING_BUFFER_H */
