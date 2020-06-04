#ifndef PQNB_H
#define PQNB_H

#include <libpq-fe.h>

#include <inttypes.h>
#include <stddef.h>

/*
 * epoll max events, stack allocated
 */
#define PQNB_MAX_EVENTS 32
/* 
 * queries buffer max entries
 */
#define PQNB_MAX_QBUF 2048
/*
 * default timeout in seconds for connecting or reconnecting
 */
#define PQNB_DEFAULT_CONNECT_TIMEOUT 5
/*
 * query default timeout
 */
#define PQNB_DEFAULT_QUERY_TIMEOUT 5

struct PQNB_pool;
/**
 * returns NULL on allocation errors / configuration problems
 */
struct PQNB_pool *
PQNB_pool_init(const char *conninfo, uint16_t num_connections);
/*
 * deallocates everything
 */
void
PQNB_pool_free(struct PQNB_pool *pool);
/**
 * returns 0 on success, -1 on error
 */
int
PQNB_pool_run(struct PQNB_pool *pool);
/*
 * used for querying pool info
 */
enum PQNB_pool_info_type
{
    PQNB_INFO_EPOLL_FD = 0,
};
/*
 * pool info
 */
union PQNB_pool_info
{
    int epoll_fd;
};
/*
 * NULL if not found
 */
const union PQNB_pool_info *
PQNB_pool_get_info(struct PQNB_pool *pool, enum PQNB_pool_info_type type);
/*
 * don't call PQclear we always call this after returning from the callback,
 * this function may be called multiple times
 */
typedef void (*PQNB_query_callback)(PGresult *pg_result, void *user_data);
typedef void (*PQNB_query_timeout_callback)(void *user_data);
/**
 * returns 0 on success, -1 on error
 */
int
PQNB_pool_query(struct PQNB_pool *pool, const char *query,
                PQNB_query_callback query_callback,
                /* nullable if no query timeout is set on pool */
                PQNB_query_timeout_callback query_timeout_callback,
                const void *user_data);

#endif /* END PQNB_H */
