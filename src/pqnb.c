#include "pqnb.h"

#include "internal.h"
#include "connection.h"
#include "ring_buffer.h"

#include <libpq-fe.h>

#include <sys/epoll.h>

#include <assert.h>
#include <time.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>

struct PQNB_pool *
PQNB_pool_init(const char *conninfo, uint16_t num_connections)
{
  struct PQNB_pool *pool = calloc(1, sizeof(*pool));
  if (NULL == pool)
    return NULL;

  pool->connect_timeout = PQNB_DEFAULT_CONNECT_TIMEOUT;
  pool->query_timeout = PQNB_DEFAULT_QUERY_TIMEOUT;

  pool->queries_buffer = PQNB_ring_buffer_init(PQNB_MAX_QBUF, 
                                               sizeof(struct PQNB_query_request));
  if (NULL == pool->queries_buffer)
    goto cleanup;

  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (-1 == epoll_fd)
    goto cleanup;
  pool->epoll_fd = epoll_fd;

  pool->connections = calloc(num_connections, sizeof(*pool->connections));
  if (NULL == pool->connections)
    goto cleanup;

  for (int i = 0; i < num_connections; i++)
    {
      struct PQNB_connection *conn = PQNB_connection_init(pool, conninfo);
      if (NULL == conn)
        goto cleanup;
      pool->connections[i] = conn;
      pool->num_connections++;
      if (-1 == PQNB_connection_begin_polling(conn))
        goto cleanup;
    }

  return pool;
cleanup:
  if (0 < pool->num_connections)
    {
      for (int j = pool->num_connections; j >= 0; --j)
        PQNB_connection_free(pool->connections[j]);
    }
  if (NULL != pool->queries_buffer)
    PQNB_ring_buffer_free(pool->queries_buffer);
  if (NULL != pool->connections)
    free(pool->connections);
  free(pool);
  return NULL;
}

void
PQNB_pool_free(struct PQNB_pool *pool)
{
  for (int i = 0; i < pool->num_connections; i++)
    PQNB_connection_free(pool->connections[i]);
  PQNB_ring_buffer_free(pool->queries_buffer);
  free(pool->connections);
  free(pool);
}

int
PQNB_pool_run(struct PQNB_pool *pool)
{
  struct PQNB_connection *conn, *next;
  struct PQNB_query_request *query_request;
  struct timespec ts;
  time_t now;
  struct epoll_event events[PQNB_MAX_EVENTS];
  int num_events;

  num_events = PQNB_MAX_EVENTS;

  if (-1 == clock_gettime(CLOCK_MONOTONIC, &ts))
    return -1;
  now = ts.tv_sec;

  while (PQNB_MAX_EVENTS == num_events)
    {
      num_events = epoll_wait(pool->epoll_fd, events,
                              PQNB_MAX_EVENTS, 0);
      if (-1 == num_events)
        {
          if (EINTR == num_events)
            continue;
          return -1;
        }
      for (int i = 0; i < num_events; i++)
        {
          conn = events[i].data.ptr;
          assert(NULL != conn);

          conn->last_activity = now;

          if ((EPOLLERR | EPOLLRDHUP) & events[i].events
              && CONN_RECONNECTING != conn->action)
            {
              PQNB_connection_reset(conn);
              continue;
            }

          if (EPOLLOUT & events[i].events)
            conn->writable = 1;
          if (EPOLLIN & events[i].events)
            conn->readable = 1;

          if (CONN_CONNECTING == conn->action)
            {
              if (CONN_POLL_INIT == conn->poll
                  && conn->writable == 1)
                PQconnectPoll(conn->pg_conn);

              if (CONN_POLL_READ == conn->poll
                  && conn->readable == 0)
                continue;
              if (CONN_POLL_WRITE == conn->poll
                  && conn->writable == 0)
                continue;

              switch(PQconnectPoll(conn->pg_conn))
                {
                case PGRES_POLLING_OK:
                  conn->poll = CONN_POLL_OK;
                  conn->action = CONN_IDLE;
                  conn->readable = 0;
                  PQNB_connecting_remove(pool->connecting_head,
                                         pool->connecting_tail, conn);
                  break;
                case PGRES_POLLING_READING:
                  conn->poll = CONN_POLL_READ;
                  conn->readable = 0;
                  break;
                case PGRES_POLLING_WRITING:
                  conn->poll = CONN_POLL_WRITE;
                  conn->writable = 0;
                  break;
                case PGRES_POLLING_FAILED:
                  PQNB_connection_reset(conn);
                  break;
                default:
                  break;
                }
            }

          if (CONN_RECONNECTING == conn->action)
            {
              if (CONN_POLL_INIT == conn->poll
                  && conn->writable == 1)
                PQresetPoll(conn->pg_conn);

              if (CONN_POLL_READ == conn->poll
                  && conn->readable == 0)
                continue;
              if (CONN_POLL_WRITE == conn->poll
                  && conn->writable == 0)
                continue;

              switch(PQresetPoll(conn->pg_conn))
                {
                case PGRES_POLLING_OK:
                  conn->poll = CONN_POLL_OK;
                  conn->action = CONN_IDLE;
                  conn->readable = 0;
                  PQNB_connecting_remove(pool->connecting_head,
                                         pool->connecting_tail,
                                         conn);
                  break;
                case PGRES_POLLING_READING:
                  conn->poll = CONN_POLL_READ;
                  conn->readable = 0;
                  break;
                case PGRES_POLLING_WRITING:
                  conn->poll = CONN_POLL_WRITE;
                  conn->writable = 0;
                  break;
                default:
                  break;
                }
            }

          if (CONN_FLUSHING == conn->action)
            {
              if (conn->readable)
                {
                  if (-1 == PQNB_connection_read(conn))
                    {
                      PQNB_connection_reset(conn);
                      continue;
                    }
                }
              if (conn->writable)
                {
                  const int res = PQNB_connection_write(conn);
                  if (0 == res)
                    conn->action = CONN_QUERYING;
                  else if (-1 == res)
                    {
                      PQNB_connection_reset(conn);
                      continue;
                    }
                }
            }

          if (CONN_QUERYING == conn->action
              && conn->readable)
            {
              if (-1 == PQNB_connection_read(conn))
                {
                  PQNB_connection_reset(conn);
                  continue;
                }
              if (0 == PQisBusy(conn->pg_conn))
                {
                  PGresult *result = NULL;
                  while(NULL != 
                        (result = PQgetResult(conn->pg_conn)))
                    {
                      conn->query_cb(result, conn->user_data,
                                     NULL, false);
                      PQclear(result);
                    }
                  PQNB_querying_remove(pool->querying_head,
                                       pool->querying_tail,
                                       conn);
                  conn->action = CONN_IDLE;
                  PQNB_connection_clear_data(conn);
                }
            }

          if (CONN_IDLE == conn->action
              && conn->writable)
            {
              if (PQNB_ring_buffer_empty(pool->queries_buffer))
                {
                  PQNB_idle_push(pool->idle_head,
                                 pool->idle_tail, conn);
                }
              else
                {
                  PQNB_connection_query(conn, 
                      PQNB_ring_buffer_pop(pool->queries_buffer));
                }
            }
        }
    }

  next = pool->connecting_head;
  while (NULL != (conn = next))
    {
      if (now - conn->last_activity < pool->connect_timeout)
        break;
      next = conn->next_connecting;
      conn->last_activity = now;
      PQNB_connection_reset(conn);
    }

  next = pool->querying_head;
  while (NULL != (conn = next))
    {
      if (now - conn->last_activity < pool->query_timeout)
        break;
      next = conn->next_querying;
      conn->last_activity = now;
      /*
       * libpq doesn't support non blocking query cancellation
       * so we reset the connection
       */
      conn->action = CONN_CANCELLING;
      conn->query_cb(NULL, conn->user_data, NULL, true);
      PQNB_connection_reset(conn);
    }

  while(NULL != 
        (query_request 
         = PQNB_ring_buffer_tail(pool->queries_buffer)))
    {
      if (now - query_request->enqueued_at < pool->query_timeout)
        break;
      query_request->query_cb(NULL, query_request->user_data,
                              NULL, true);
      PQNB_ring_buffer_pop(pool->queries_buffer);
    }
  return 0;
}

const union PQNB_pool_info *
PQNB_pool_get_info(struct PQNB_pool *pool, 
                   enum PQNB_pool_info_type info_type)
{
  if (PQNB_INFO_EPOLL_FD == info_type)
    return (const union PQNB_pool_info*) &pool->epoll_fd;
  else
    return NULL;
}

int
PQNB_pool_query(struct PQNB_pool *pool, const char *query,
                PQNB_query_cb query_cb, const void *user_data)
{
  struct PQNB_query_request query_request;
  struct PQNB_connection *conn;
  struct timespec ts;

  query_request.query = (char*) query;
  query_request.query_cb = query_cb;
  query_request.user_data = (void*) user_data;

  if (-1 == clock_gettime(CLOCK_MONOTONIC, &ts))
    return -1;
  query_request.enqueued_at = ts.tv_sec;

  conn = pool->idle_head;
  if (NULL == conn)
      return PQNB_ring_buffer_push(pool->queries_buffer,
                                   &query_request);
  else
    {
      PQNB_idle_remove(pool->idle_head,
                       pool->idle_tail, conn);
      return PQNB_connection_query(conn, &query_request);
    }
}
