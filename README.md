# libpqnb
Postgres non-blocking libpq connection pool

# How to build
make libpqnb.so

# Usage
Initialize the connection pool:
struct PQNB_pool *pool;
char *conninfo = "postgresql:///yourdbname?host=/var/run/postgresql";
uint16_t num_connections = 32;
pool = PQNB_pool_init(conninfo, num_connections);

Get pool epoll file descriptor:
const union PQNB_pool_info *info;
info = PQNB_pool_get_info(pool, PQNB_INFO_EPOLL_FD);

Run pool:
PQNB_pool_run(pool);
This call doesn't block, you may select/epoll_wait the pool epoll_fd,
see test.c

Run a query:
PQNB_pool_query(pool, "SELECT * FROM version()", query_callback, /*user_data*/ NULL);
