PG_INCLUDEDIR = $(shell pg_config --includedir)
CFLAGS +=-Wall -Wextra -Werror -I. -I$(PG_INCLUDEDIR) -fPIC -O3 -std=gnu11
LDFLAGS +=-shared

TEST_CFLAGS +=-Wall -Wextra -Werror -I. -I$(PG_INCLUDEDIR) -O3 -std=gnu11
TEST_LDFLAGS +=-L. -lpqnb -lpq

valgrind: valgrind.sh test
	sh ./valgrind.sh
test: libpqnb.so test.c
	$(CC) $(TEST_CFLAGS) -o test test.c $(TEST_LDFLAGS)
libpqnb.so: pqnb.o ring_buffer.o
	$(CC) $(LDFLAGS) -o libpqnb.so pqnb.o ring_buffer.o
pqnb.o: pqnb.c pqnb.h
	$(CC) $(CFLAGS) -c pqnb.c
ring_buffer.o: ring_buffer.c ring_buffer.h
	$(CC) $(CFLAGS) -c ring_buffer.c
.PHONY:
clean:
	$(RM) -fv *.o *.so test valgrind-out.txt
