#ifndef PQNB_INTERNAL_H
#define PQNB_INTERNAL_H

#include "pqnb.h"
#include "ring_buffer.h"

#include <libpq-fe.h>
#include <inttypes.h>
#include <time.h>

enum PQNB_connection_action
{
  CONN_CONNECTING = 0,
  CONN_RECONNECTING,
  CONN_IDLE,
  CONN_FLUSHING,
  CONN_QUERYING
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
   * user data attached to query, NULL after passing all results
   */
  void *user_data;
  /*
   * last epoll activity
   */
  time_t last_activity;
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
  /*
   * buffer of idle connections
   */
  struct PQNB_ring_buffer *idle_connections;
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
   * the sql query
   */
  char *query;
  /*
   * user defined callback, we call it when we have
   * PGresult's available for reading
   */
  PQNB_query_callback query_callback;
  /*
   * user defined data
   */
  void *user_data;
};

#endif /* ~PQNB_INTERNAL_H */
