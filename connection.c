#include "internal.h"

#include "connection.h"

#include <libpq-fe.h>

#include <sys/epoll.h>
#include <time.h>
#include <stdlib.h>

struct PQNB_connection *
PQNB_connection_init(struct PQNB_pool *pool, const char *conninfo)
{
  struct timespec ts;

  PGconn *pg_conn = PQconnectStart(conninfo);
  if (NULL == pg_conn)
    return NULL;
  if (CONNECTION_BAD == PQstatus(pg_conn))
    goto cleanup;
  PQsetnonblocking(pg_conn, 1);
  if (-1 == clock_gettime(CLOCK_MONOTONIC, &ts))
    goto cleanup;
  struct PQNB_connection *conn = calloc(1, sizeof(*conn));
  if (NULL == conn)
    goto cleanup;

  conn->action = CONN_CONNECTING;
  conn->pool = pool;
  conn->pg_conn = pg_conn;
  conn->last_activity = ts.tv_sec;

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
  PQNB_connection_reset_data(conn);

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

int
PQNB_connection_cancel_command(struct PQNB_connection *conn)
{
  if (conn->writable)
    {
      if (-1 == PQrequestCancel(conn->pg_conn))
        return -1;
      conn->action = CONN_IDLE;
      conn->writable = 0;
    }
  else
    {
      conn->action = CONN_CANCELLING;
    }
  PQNB_connection_reset_data(conn);
  return 0;
}

void
PQNB_connection_reset_data(struct PQNB_connection *conn)
{
  conn->query_callback = NULL;
  conn->query_timeout_callback = NULL;
  conn->user_data = NULL;
}
