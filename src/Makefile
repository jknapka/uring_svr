DBGFLAGS = -g

all: uring_svr

uring_svr: uring_svr.o ../../liburing/src/liburing.so.1.0.1

uring_svr.o: uring_svr.c

clean:
	rm -f *.o uring_svr
