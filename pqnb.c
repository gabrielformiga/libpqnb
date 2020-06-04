#include "pqnb.h"

#include "internal.h"
#include "connection.h"
#include "ring_buffer.h"

#include <libpq-fe.h>

#include <sys/epoll.h>

#include <time.h>
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
  pool->connect_timeout = PQNB_DEFAULT_CONNECT_TIMEOUT;
  pool->query_timeout = PQNB_DEFAULT_QUERY_TIMEOUT;
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
  struct timespec ts;
  struct epoll_event events[PQNB_MAX_EVENTS];
  int num_events = PQNB_MAX_EVENTS;

  if (-1 == clock_gettime(CLOCK_MONOTONIC, &ts))
    return -1;

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

          conn->last_activity = ts.tv_sec;

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
                  PQNB_connection_reset_data(conn);
                }
            }

          if (CONN_CANCELLING == conn->action
              && conn->writable)
            {
              PQNB_connection_cancel_command(conn);
              continue;
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

  /* 
   * TODO: queue or list of activities that may timeout
   * instead of running all the array, probably instead
   * of checking the current action we may create a
   * timeout member on the connection
   */
  for (int i = 0; i < pool->num_connections; i++)
    {
      struct PQNB_connection *conn = pool->connections[i];
      if (NULL == conn)
        return -1;
      time_t last_activity = conn->last_activity;
      if ((conn->action == CONN_CONNECTING
           || conn->action == CONN_RECONNECTING)
          && ts.tv_sec - last_activity > pool->connect_timeout)
        PQNB_connection_reset(conn);
      if (pool->query_timeout > 0
          && (conn->action == CONN_QUERYING
              || conn->action == CONN_FLUSHING)
          && ts.tv_sec - last_activity > pool->query_timeout)
        {
          conn->query_timeout_callback(conn->user_data);
          PQNB_connection_cancel_command(conn);
        }
    }
  if (pool->query_timeout > 0
      && PQNB_ring_buffer_not_empty(pool->queries_buffer))
    {
      struct PQNB_query_request *query_request;
      while((query_request = 
             PQNB_ring_buffer_tail(pool->queries_buffer))
            != NULL)
        {
          if (ts.tv_sec - query_request->enqueued_at > pool->query_timeout)
            {
              query_request->query_timeout_callback(query_request->user_data);
              PQNB_ring_buffer_pop(pool->queries_buffer);
            }
          else
            break; /* no need to check the others */
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
                PQNB_query_timeout_callback query_timeout_callback,
                const void *user_data)
{
  struct PQNB_query_request query_request;
  struct PQNB_connection *conn;

  query_request.query = (char*) query;
  query_request.query_callback = query_callback;
  query_request.user_data = (void*) user_data;
  query_request.query_timeout_callback = query_timeout_callback;

  conn = PQNB_pop_idle_connection(pool);
  if (NULL == conn)
    {
      if (pool->query_timeout > 0)
        {
          struct timespec ts;
          if (-1 == clock_gettime(CLOCK_MONOTONIC, &ts))
            return -1;
          query_request.enqueued_at = ts.tv_sec;
        }
      return PQNB_ring_buffer_push(pool->queries_buffer, &query_request);
    }
  else
    {
      query_request.enqueued_at = 0;
      return PQNB_connection_query(conn, &query_request);
    }
}
