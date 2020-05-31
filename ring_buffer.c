#include "ring_buffer.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct PQNB_ring_buffer
{
  void *buffer;     
  void *buffer_end; 
  size_t capacity;
  size_t count;  
  size_t sz;     
  void *head;
  void *tail;
};

struct PQNB_ring_buffer*
PQNB_ring_buffer_init(size_t capacity, size_t sz)
{
  struct PQNB_ring_buffer *ring_buffer = malloc(sizeof(*ring_buffer));
  if (NULL == ring_buffer)
    return NULL;
  ring_buffer->buffer = malloc(capacity * sz);
  if(ring_buffer->buffer == NULL)
    {
      free(ring_buffer);
      return NULL;
    }
  ring_buffer->buffer_end = (char *) ring_buffer->buffer + capacity * sz;
  ring_buffer->capacity = capacity;
  ring_buffer->count = 0;
  ring_buffer->sz = sz;
  ring_buffer->head = ring_buffer->buffer;
  ring_buffer->tail = ring_buffer->buffer;
  return ring_buffer;
}

void
PQNB_ring_buffer_free(struct PQNB_ring_buffer *ring_buffer)
{
  free(ring_buffer->buffer);
  free(ring_buffer);
}

bool
PQNB_ring_buffer_empty(struct PQNB_ring_buffer *ring_buffer)
{
  return 0 == ring_buffer->count;
}

bool
PQNB_ring_buffer_not_empty(struct PQNB_ring_buffer *ring_buffer)
{
  return 0 < ring_buffer->count;
}

int
PQNB_ring_buffer_push(struct PQNB_ring_buffer *ring_buffer, const void *item)
{
  if(ring_buffer->count == ring_buffer->capacity)
    return -1;
  memcpy(ring_buffer->head, item, ring_buffer->sz);
  ring_buffer->head = (char*) ring_buffer->head + ring_buffer->sz;
  if(ring_buffer->head == ring_buffer->buffer_end)
    ring_buffer->head = ring_buffer->buffer;
  ring_buffer->count++;
  return 0;
}

void*
PQNB_ring_buffer_pop(struct PQNB_ring_buffer *ring_buffer)
{
  if(0 == ring_buffer->count)
    return NULL;
  void *item = ring_buffer->tail;
  ring_buffer->tail = (char*) ring_buffer->tail + ring_buffer->sz;
  if(ring_buffer->tail == ring_buffer->buffer_end)
    ring_buffer->tail = ring_buffer->buffer;
  ring_buffer->count--;
  return item;
}
