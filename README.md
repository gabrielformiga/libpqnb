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
/* just a example struct */   
struct query_counter { uint64_t count; };   
   
/* callback */   
void   
query_callback(PGresult *res,   
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
   
/* querying */
PQNB_pool_query(pool, "SELECT * FROM version()", query_callback,
                &counter);  
```  
