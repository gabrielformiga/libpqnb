#include "pqnb.h"

#include <libpq-fe.h>

#include <sys/epoll.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>

#define CONNINFO "postgresql:///yourdbname?host=/var/run/postgresql"
#define QUERY "SELECT * FROM version()"
#define TEST_TIME_SEC 30
#define NUM_CONNECTIONS 32

struct query_counter { uint64_t count; };

#if 0
static void
test_print_res(PGresult *res)
{
  int num_fields = PQnfields(res);
  for (int i = 0; i < num_fields; i++) {
      const int ftype = PQftype(res, i);
      printf("fname %s ftype %d\n", PQfname(res, i), ftype);
  }
  /* next, print out the rows */
  int num_rows = PQntuples(res);
  for (int i = 0; i < num_rows; i++) {
      for (int j = 0; j < num_fields; j++)
        printf("%s\t", PQgetvalue(res, i, j));
      printf("\n");
  }
}
#endif /* ~comment */

void
test_query_cb(PGresult *res,
              void *user_data,
              char *error_msg,
              bool timeout)
{
  struct query_counter *queries_counter;

  /*
   * ignoring compiler warnings
   */
  (void) res;

  if (timeout)
    {
      printf("timeout\n");
      return;
    }

  if (NULL != error_msg)
    {
      printf("%s", error_msg);
      return;
    }

  if (PGRES_TUPLES_OK == PQresultStatus(res))
    {
      assert(NULL != user_data);
      queries_counter = user_data;
      queries_counter->count++;
    }
  else
    printf("query failed\n");
}

/*
 * just querying for a minute
 */
int
main(void)
{
  struct PQNB_pool *pool;
  const union PQNB_pool_info *info;
  int epoll_fd, res;
  struct epoll_event ev, evs[1];
  struct query_counter counter;
  time_t end;

  pool = PQNB_pool_init(CONNINFO, NUM_CONNECTIONS);
  assert(NULL != pool);
  info = PQNB_pool_get_info(pool, PQNB_INFO_EPOLL_FD);
  assert(NULL != info);
  assert(info->epoll_fd != -1);
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  assert(epoll_fd != -1);
  ev.events = EPOLLIN;
  ev.data.ptr = NULL;
  res = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, info->epoll_fd, &ev);
  assert(-1 != res);
  counter.count = 0;

  /* filling query buffer */
  while(-1 != PQNB_pool_query(pool, QUERY, test_query_cb, &counter));

  end = time(0) + TEST_TIME_SEC;
  for (;;)
    {
      if (end <= time(0))
        break;
      res = epoll_wait(epoll_fd, evs, 1, 1000);
      if (res == -1)
        break;
      if (PQNB_pool_run(pool) == -1)
        break;
      /* filling query buffer */
      while(-1 != PQNB_pool_query(pool, QUERY, test_query_cb, &counter));
    }

  PQNB_pool_free(pool);

  printf("total queries: %ld\n", counter.count);

  return 0;
}
