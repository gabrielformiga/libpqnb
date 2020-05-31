#include "pqnb.h"

#include "ring_buffer.h"

#include <libpq-fe.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>

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
   * currently used for reconnections
   */
  int reconnection_timerfd;
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

/*
 * helper for wrapping a pointer
 */
struct PQNB_pointer_wrapper { void *ptr; };

static struct PQNB_connection *
PQNB_connection_init(struct PQNB_pool *pool, const char *conninfo)
{
  PGconn *pg_conn = PQconnectStart(conninfo);
  if (NULL == pg_conn)
    return NULL;
  if (CONNECTION_BAD == PQstatus(pg_conn))
    goto cleanup;
  PQsetnonblocking(pg_conn, 1);

  struct PQNB_connection *conn = calloc(1, sizeof(*conn));
  if (NULL == conn)
    goto cleanup;

  conn->reconnection_timerfd = -1;
  conn->pool = pool;
  conn->pg_conn = pg_conn;
  conn->action = CONN_CONNECTING;

  return conn;
cleanup:
  PQfinish(pg_conn);
  return NULL;
}

static void
PQNB_connection_free(struct PQNB_connection *conn)
{
  PQfinish(conn->pg_conn);
  free(conn);
}

static int32_t
PQNB_connection_begin_polling(struct PQNB_connection *conn)
{
  const int conn_fd = PQsocket(conn->pg_conn);
  struct epoll_event event;

  event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
  event.data.ptr = conn;

  const int res = epoll_ctl(conn->pool->epoll_fd,
                            EPOLL_CTL_ADD, conn_fd, &event);
  conn->poll = CONN_POLL_INIT;
  return res;
}

static int32_t
PQNB_connection_reset(struct PQNB_connection *conn)
{
  /*
   * reset connection data
   */
  conn->action = CONN_RECONNECTING;
  conn->writable = 0;
  conn->readable = 0;
  conn->user_data = NULL;
  conn->query_callback = NULL;

  if (-1 == conn->reconnection_timerfd)
    {
      conn->reconnection_timerfd = timerfd_create(CLOCK_MONOTONIC,
                                                  TFD_NONBLOCK | TFD_CLOEXEC);
      if (-1 == conn->reconnection_timerfd)
        return -1;
      struct epoll_event ev;
      ev.events = EPOLLIN;
      ev.data.ptr = conn;
      if (-1 == epoll_ctl(conn->pool->epoll_fd, EPOLL_CTL_ADD,
                          conn->reconnection_timerfd, &ev))
        return -1;
      struct itimerspec ts;
      ts.it_value.tv_sec = 10;
      ts.it_value.tv_nsec = 0;
      ts.it_interval.tv_sec = 10;
      ts.it_interval.tv_nsec = 0;
      if (-1 == timerfd_settime(conn->reconnection_timerfd, 0, &ts, NULL))
        return -1;
    }

  if (0 == PQresetStart(conn->pg_conn))
    return -1;
  if (CONNECTION_BAD == PQstatus(conn->pg_conn))
    return -1;
  PQsetnonblocking(conn->pg_conn, 1);

  return PQNB_connection_begin_polling(conn);
}

static int32_t
PQNB_connection_read(struct PQNB_connection *conn)
{
  int32_t ret;

  ret = PQconsumeInput(conn->pg_conn);
  conn->readable = 0;
  return ret;
}

static int32_t
PQNB_connection_write(struct PQNB_connection *conn)
{
  int32_t ret;

  ret = PQflush(conn->pg_conn);
  conn->writable = 0;
  return ret;
}

static int32_t
PQNB_connection_query(struct PQNB_connection *conn,
                      struct PQNB_query_request *req)
{
  int32_t res;

  if (0 == PQsendQuery(conn->pg_conn, req->query))
    {
      PQNB_connection_reset(conn);
      return -1;
    }
  res = PQNB_connection_write(conn);
  if (0 == res)
    conn->action = CONN_QUERYING;
  else if (1 == res)
    conn->action = CONN_FLUSHING;
  else {
      PQNB_connection_reset(conn);
      return -1;
  }
  conn->query_callback = req->query_callback;
  conn->user_data = req->user_data;
  return 0;
}

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
static int16_t
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

int32_t
PQNB_pool_run(struct PQNB_pool *pool)
{
  struct epoll_event events[PQNB_MAX_EVENTS];
  int num_events = PQNB_MAX_EVENTS;

  while (PQNB_MAX_EVENTS == num_events)
    {
      num_events = epoll_wait(pool->epoll_fd, events, PQNB_MAX_EVENTS, 0);
      if (-1 == num_events)
        return -1;

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
              const int32_t res = PQNB_connection_write(conn);
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

int32_t
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
