#include "internal.h"

#include <libpq-fe.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <stdlib.h>

struct PQNB_connection *
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

void
PQNB_connection_free(struct PQNB_connection *conn)
{
  PQfinish(conn->pg_conn);
  free(conn);
}

int
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

int
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

int
PQNB_connection_read(struct PQNB_connection *conn)
{
  int ret;

  ret = PQconsumeInput(conn->pg_conn);
  conn->readable = 0;
  return ret;
}

int
PQNB_connection_write(struct PQNB_connection *conn)
{
  int ret;

  ret = PQflush(conn->pg_conn);
  conn->writable = 0;
  return ret;
}

int
PQNB_connection_query(struct PQNB_connection *conn,
                      struct PQNB_query_request *req)
{
  int res;

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
