CFLAGS +=-Wall -Wextra -Werror -I. -Iinclude -I$(PG_INCLUDEDIR) -O3 -flto -std=gnu11 -fPIC
LDFLAGS +=-shared -O3 -flto

TEST_CFLAGS +=-Wall -Wextra -Werror -I. -Iinclude -I$(PG_INCLUDEDIR) -O3 -flto -std=gnu11 -fPIC
TEST_LDFLAGS +=-L. -lpqnb -lpq

ifeq ($(PG_INCLUDEDIR),)
PG_INCLUDEDIR = $(shell pg_config --includedir)
endif

valgrind: valgrind.sh sample/test
	sh ./valgrind.sh
test: libpqnb.so sample/test.c
	$(CC) $(TEST_CFLAGS) -o test sample/test.c $(TEST_LDFLAGS)
libpqnb.so: src/pqnb.o src/connection.o src/ring_buffer.o
	$(CC) $(LDFLAGS) -o libpqnb.so src/pqnb.o src/connection.o src/ring_buffer.o
src/pqnb.o: src/pqnb.c include/pqnb.h src/internal.h src/connection.h
	$(CC) $(CFLAGS) -o src/pqnb.o -c src/pqnb.c
src/connection.o: src/connection.c src/connection.h src/internal.h
	$(CC) $(CFLAGS) -o src/connection.o -c src/connection.c
src/ring_buffer.o: src/ring_buffer.c src/ring_buffer.h
	$(CC) $(CFLAGS) -o src/ring_buffer.o -c src/ring_buffer.c
.PHONY:
clean:
	$(RM) -fv src/*.o sample/*.o *.so test valgrind-out.txt
