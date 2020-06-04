# libpqnb
Postgres non-blocking libpq connection pool

# How to build
```
make libpqnb.so
```  
# Usage
Initialize the connection pool:  
```c
struct PQNB_pool *pool;  
const char conninfo[] = "postgresql:///yourdbname?host=/var/run/postgresql";  
uint16_t num_connections = 32;  
pool = PQNB_pool_init(conninfo, num_connections);  
```  
Get pool epoll file descriptor:  
```c
const union PQNB_pool_info *info;  
info = PQNB_pool_get_info(pool, PQNB_INFO_EPOLL_FD);  
```  
Run pool:  
```c
/* This call doesn't block, you may select/epoll_wait the pool epoll_fd */  
/* see sample/test.c */  
PQNB_pool_run(pool);  
```  
  
Run a query:  
```c
PQNB_pool_query(pool, "SELECT * FROM version()", query_callback,  
                query_timeout_callback, /*user_data*/ NULL);  
```  
