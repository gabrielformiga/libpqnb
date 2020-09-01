#ifndef PQNB_CONNECTION_H
#define PQNB_CONNECTION_H

#include "internal.h"

struct PQNB_connection *
PQNB_connection_init(struct PQNB_pool *pool, const char *conninfo);

void
PQNB_connection_free(struct PQNB_connection *conn);

int
PQNB_connection_begin_polling(struct PQNB_connection *conn);

int
PQNB_connection_reset(struct PQNB_connection *conn);

int
PQNB_connection_read(struct PQNB_connection *conn);

int
PQNB_connection_write(struct PQNB_connection *conn);

int
PQNB_connection_query(struct PQNB_connection *conn,
                      struct PQNB_query_request *req);

void
PQNB_connection_clear_data(struct PQNB_connection *conn);

void
PQNB_connection_cb_err(struct PQNB_connection *conn);

#endif /* ~PQNB_CONNECTION_H */
