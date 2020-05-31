#include "pqnb.h"

#include "internal.h"
#include "connection.h"
#include "ring_buffer.h"

#include <asm-generic/errno-base.h>
#include <libpq-fe.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>

/*
 * helper for wrapping a pointer
 */
struct PQNB_pointer_wrapper { void *ptr; };

/* pops a writable connection */
static struct PQNB_connection *
PQNB_pop_idle_connection(struct PQNB_pool *pool)
{
  struct PQNB_pointer_wrapper *w;

  w = PQNB_ring_buffer_pop(pool->idle_connections);
  if (NULL == w)
    return NULL;
  return (struct PQNB_connection*) w->ptr;
}

/*
 * pushes an idle connection in a pointer wrapper
 */
static int
PQNB_push_idle_connection(struct PQNB_pool *pool,
                          struct PQNB_connection *conn)
{
  struct PQNB_pointer_wrapper w = { conn };
  return PQNB_ring_buffer_push(pool->idle_connections, &w);
}

struct PQNB_pool *
PQNB_pool_init(const char *conninfo, uint16_t num_connections)
{
  struct PQNB_pool *pool = calloc(1, sizeof(*pool));
  if (NULL == pool)
    return NULL;

  pool->idle_connections = PQNB_ring_buffer_init(num_connections, 
                                                 sizeof(struct PQNB_pointer_wrapper));
  if (NULL == pool->idle_connections)
    goto cleanup;

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
  if (NULL != pool->idle_connections)
    PQNB_ring_buffer_free(pool->idle_connections);
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
  PQNB_ring_buffer_free(pool->idle_connections);
  PQNB_ring_buffer_free(pool->queries_buffer);
  free(pool->connections);
  free(pool);
}

int
PQNB_pool_run(struct PQNB_pool *pool)
{
  struct epoll_event events[PQNB_MAX_EVENTS];
  int num_events = PQNB_MAX_EVENTS;

  while (PQNB_MAX_EVENTS == num_events)
    {
      num_events = epoll_wait(pool->epoll_fd, events, PQNB_MAX_EVENTS, 0);
      if (-1 == num_events)
        {
          if (EINTR == num_events)
            continue;
          return -1;
        }
      for (int i = 0; i < num_events; i++)
        {
          struct PQNB_connection *conn = events[i].data.ptr;
          if (NULL == conn)
            return -1;

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
              uint64_t expired_count;
              /*
               * not the ideal solution, but works for now..
               */
              if (-1 == read(conn->reconnection_timerfd, &expired_count,
                             sizeof(expired_count)))
                {
                  if (EINTR == errno)
                    continue;
                  if (EAGAIN != errno)
                    return -1;
                }
              else
                {
                  PQNB_connection_reset(conn);
                  continue;
                }

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
                  if (-1 == close(conn->reconnection_timerfd))
                    {
                      return -1;
                    }
                  conn->reconnection_timerfd = -1;
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
              const int res = PQNB_connection_write(conn);
              if (0 == res)
                conn->action = CONN_QUERYING;
              else if (-1 == res)
                {
                  PQNB_connection_reset(conn);
                  continue;
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
              /*
               * will not block
               */
              if (0 == PQisBusy(conn->pg_conn))
                {
                  PGresult *result = NULL;
                  while((result = PQgetResult(conn->pg_conn)) != NULL)
                    {
                      conn->query_callback(result, conn->user_data);
                      PQclear(result);
                    }
                  conn->action = CONN_IDLE;
                  conn->query_callback = NULL;
                  conn->user_data = NULL;
                }
            }

          if (CONN_IDLE == conn->action
              && conn->writable)
            {
              if (PQNB_ring_buffer_empty(pool->queries_buffer))
                PQNB_push_idle_connection(pool, conn);
              else
                PQNB_connection_query(conn, 
                                      PQNB_ring_buffer_pop(pool->queries_buffer));
            }
        }
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
                PQNB_query_callback query_callback,
                const void *user_data)
{
  struct PQNB_query_request query_request;
  struct PQNB_connection *conn;

  query_request.query = (char*) query;
  query_request.query_callback = query_callback;
  query_request.user_data = (void*) user_data;

  conn = PQNB_pop_idle_connection(pool);
  if (NULL == conn)
    return PQNB_ring_buffer_push(pool->queries_buffer, &query_request);
  else
    return PQNB_connection_query(conn, &query_request);
}
