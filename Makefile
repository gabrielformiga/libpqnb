PG_INCLUDEDIR = $(shell pg_config --includedir)

CFLAGS +=-Wall -Wextra -Werror -I. -Iinclude -I$(PG_INCLUDEDIR) -O3 -flto -std=gnu11 -fPIC
LDFLAGS +=-shared -O3 -flto

TEST_CFLAGS +=-Wall -Wextra -Werror -I. -Iinclude -I$(PG_INCLUDEDIR) -O3 -flto -std=gnu11
TEST_LDFLAGS +=-L. -lpqnb -lpq

valgrind: valgrind.sh sample/test
	sh ./valgrind.sh
sample/test: libpqnb.so sample/test.c
	$(CC) $(TEST_CFLAGS) -o sample/test sample/test.c $(TEST_LDFLAGS)
libpqnb.so: pqnb.o connection.o ring_buffer.o
	$(CC) $(LDFLAGS) -o libpqnb.so pqnb.o connection.o ring_buffer.o
pqnb.o: pqnb.c include/pqnb.h internal.h connection.h
	$(CC) $(CFLAGS) -c pqnb.c
connection.o: connection.c connection.h internal.h
	$(CC) $(CFLAGS) -c connection.c
ring_buffer.o: ring_buffer.c ring_buffer.h
	$(CC) $(CFLAGS) -c ring_buffer.c
.PHONY:
clean:
	$(RM) -fv *.o *.so sample/test sample/*.o valgrind-out.txt
