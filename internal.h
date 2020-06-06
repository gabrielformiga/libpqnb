#ifndef PQNB_INTERNAL_H
#define PQNB_INTERNAL_H

#include "pqnb.h"
#include "ring_buffer.h"

#include <libpq-fe.h>

#include <assert.h>
#include <inttypes.h>
#include <time.h>

#define PQNB_idle_push(head, tail, c) do {   \
    assert(NULL == (c)->next_idle);          \
    assert(NULL == (c)->prev_idle);          \
    (c)->prev_idle = (tail);                 \
    (c)->next_idle = NULL;                   \
    if (NULL == (head))                      \
        (head) = (c);                        \
    else                                     \
      (tail)->next_idle = (c);               \
    (tail) = (c);                            \
} while (0)                                  \

#define PQNB_idle_remove(head, tail, c) do {      \
  if (NULL == (c)->prev_idle)                     \
    (head) = (c)->next_idle;                      \
  else                                            \
    (c)->prev_idle->next_idle = (c)->next_idle;   \
  if (NULL == (c)->next_idle)                     \
    (tail) = (c)->prev_idle;                      \
  else                                            \
    (c)->next_idle->prev_idle = (c)->prev_idle;   \
  (c)->next_idle = NULL;                          \
  (c)->prev_idle = NULL;                          \
} while(0)                                        \

#define PQNB_connecting_push(head, tail, c) do {      \
    assert(NULL == (c)->next_connecting);             \
    assert(NULL == (c)->prev_connecting);             \
    (c)->prev_connecting = (tail);                    \
    (c)->next_connecting = NULL;                      \
    if (NULL == (head))                               \
        (head) = (c);                                 \
    else                                              \
      (tail)->next_connecting = (c);                  \
    (tail) = (c);                                     \
} while (0)                                           \

#define PQNB_connecting_remove(head, tail, c) do {                    \
    if (NULL == (c)->prev_connecting)                                 \
      (head) = (c)->next_connecting;                                  \
    else                                                              \
      (c)->prev_connecting->next_connecting = (c)->next_connecting;         \
    if (NULL == (c)->next_connecting)                                 \
      (tail) = (c)->prev_connecting;                                  \
    else                                                              \
      (c)->next_connecting->prev_connecting = (c)->prev_connecting;   \
    (c)->next_connecting = NULL;                                      \
    (c)->prev_connecting = NULL;                                      \
} while(0)                                                            \

#define PQNB_querying_push(head, tail, c) do {  \
    assert(NULL == (c)->next_querying);         \
    assert(NULL == (c)->prev_querying);         \
    (c)->prev_querying = (tail);                \
    (c)->next_querying = NULL;                  \
    if (NULL == (head))                         \
        (head) = (c);                           \
    else                                        \
      (tail)->next_querying = (c);              \
    (tail) = (c);                               \
} while (0)                                     \

#define PQNB_querying_remove(head, tail, c) do {                \
  if (NULL == (c)->prev_querying)                               \
    (head) = (c)->next_querying;                                \
  else                                                          \
    (c)->prev_querying->next_querying = (c)->next_querying;     \
  if (NULL == (c)->next_querying)                               \
    (tail) = (c)->prev_querying;                                \
  else                                                          \
    (c)->next_querying->prev_querying = (c)->prev_querying;     \
  (c)->next_querying = NULL;                                    \
  (c)->prev_querying = NULL;                                    \
} while(0)                                                      \
                                              
enum PQNB_connection_action
{
  CONN_CONNECTING = 0,
  CONN_RECONNECTING,
  CONN_IDLE,
  CONN_FLUSHING,
  CONN_QUERYING,
  CONN_CANCELLING
};

enum PQNB_connection_poll
{
  CONN_POLL_INIT = 0,
  CONN_POLL_READ,
  CONN_POLL_WRITE,
  CONN_POLL_OK
};

struct PQNB_connection
{
  /*
   * last epoll activity
   */
  time_t last_activity;
  /*
   * the pool this belongs to
   */
  struct PQNB_pool *pool;
  /* 
   *  postgres connection
   */
  PGconn *pg_conn;
  /* 
   * query callback, NULL after passing all results
   */
  PQNB_query_callback query_callback;
  /*
   * called when a query timeout occurs
   */
  PQNB_query_timeout_callback query_timeout_callback;
  /*
   * user data attached to query, NULL after passing all results
   */
  void *user_data;
  /**
   * next idle connection
   */
  struct PQNB_connection *next_idle;
  /**
   * previous idle connection
   */
  struct PQNB_connection *prev_idle;
  /**
   * next (re)connecting connection
   */
  struct PQNB_connection *next_connecting;
  /**
   * previous (re)connecting connection
   */
  struct PQNB_connection *prev_connecting;
  /**
   * next querying connection
   */
  struct PQNB_connection *next_querying;
  /**
   * previous querying connection
   */
  struct PQNB_connection *prev_querying;
  /*
   * what the connection is currently doing
   */
  uint32_t action: 3;
  /* 
   * this is only used for connections and reconnections
   */
  uint32_t poll: 2;
  /*
   * if we can write without blocking
   */
  uint32_t writable: 1;
  /*
   * if we can read without blocking
   */
  uint32_t readable: 1;
};

/*
 * connection pool
 */
struct PQNB_pool
{
  /*
   * all connections
   */
  struct PQNB_connection **connections;
  /**
   * idle connections head
   */
  struct PQNB_connection *idle_head;
  /**
   * idle connections tail
   */
  struct PQNB_connection *idle_tail;
  /**
   * (re)connecting connections head
   */
  struct PQNB_connection *connecting_head;
  /**
   * (re)connecting connections tail
   */
  struct PQNB_connection *connecting_tail;
  /**
   * querying connections head
   */
  struct PQNB_connection *querying_head;
  /**
   * querying connections tail
   */
  struct PQNB_connection *querying_tail;
  /*
   * buffer of pending queries, filled if there's
   * no idle_connection and user requests a query
   */
  struct PQNB_ring_buffer *queries_buffer;
  /*
   * connection timeout in seconds for
   * connecting or reconnecting
   */
  time_t connect_timeout;
  /*
   * default query timeout
   */
  time_t query_timeout;
  /*
   * epoll file descriptor
   */
  int epoll_fd;
  /*
   * total connections number
   */
  uint16_t num_connections;
};

/* 
 * query request
 */
struct PQNB_query_request
{
  /*
   * time it was enqueued on the query buffer
   */
  time_t enqueued_at;
  /*
   * the sql query
   */
  char *query;
  /*
   * user defined callback, we call it when we have
   * PGresult's available for reading
   */
  PQNB_query_callback query_callback;
  /*
   * called when a query timeout occurs
   */
  PQNB_query_timeout_callback query_timeout_callback;
  /*
   * user defined data
   */
  void *user_data;
};

#endif /* ~PQNB_INTERNAL_H */
