/*
 * uring_svr.c
 *
 * vim: shiftwidth=4 tabstop=4 softtabstop=4 noexpandtab
 *
 * This file implements a very simple file server using the
 * Linux io_uring acynchronous I/O API.
 *
 * Implementation notes:
 *
 * - Up to MAX_CONNECTIONS connections are supported.
 * - We use a single io_uring for all async I/O requests.
 * - We allow at most one I/O request to be outstanding
 *   for any given connection, either a read or a write.
 *   We read a block into a buffer from a file, then write
 *   that same block to the connected socket.
 * - We *could* start the next read as soon as a read
 *   completes and we've started the corresponding write.
 *   But right now I just want to get something basic
 *   working.
 *
 * Copyright (c) 2019 Joseph A. Knapka. All rights reserved.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <string.h>
#include <io_uring.h>
#include <liburing.h>

/* Keep track of the data associated with a file being served. */
struct connection_rec {
	int conn_fd;
	int file_fd;
	size_t file_size;
	size_t remaining;
	off_t start_offset,offset;
	int cstat; /* 0 = idle, 1 = waiting for read, 2 = waiting for write. */
	char* io_buffer;
};

#define CONN_IDLE 0
#define CONN_READ 1
#define CONN_WRITE 2

static const int IOBLOCK_SIZE = 1024;

static const int URING_DEPTH = 32;

#define MAX_CONNECTIONS 16

static struct connection_rec connections[MAX_CONNECTIONS];

/* Ths uring instance itself. */
struct io_uring uring;

static void usage()
{
	puts("Usage: uring_svr <port>");
}

static void initConnections()
{
	int ii;
	for (ii=0; ii<MAX_CONNECTIONS; ++ii) {
		/* A conn_fd of -1 indicates an unused connection. */
		connections[ii].conn_fd = -1;
	}
}

static struct connection_rec* getConnectionRecord()
{
	int ii;
	for (ii=0; ii<MAX_CONNECTIONS; ++ii) {
		if (connections[ii].conn_fd == -1) {
			return &(connections[ii]);
		}
	}
	return 0;
}

static struct connection_rec* buildConnectionRecord(int conn_fd)
{
	char fname[1024];
	int rc, file_fd, uring_fd;
	struct stat stat_buf;
	struct connection_rec* conn_rec;

	memset(fname,0,sizeof(fname));
	/* This is half-assed, fix plz. */
	rc = read(conn_fd,(void*)fname,sizeof(fname));

	file_fd = open(fname,O_RDONLY);
	if (file_fd < 0) {
		perror("opening file");
		return 0;
	}
	rc = fstat(file_fd,&stat_buf);
	if (rc < 0) {
		perror("get file size");
		return 0;
	}

	conn_rec = getConnectionRecord();
	if (!conn_rec) {
		return 0;
	}

	conn_rec->conn_fd = conn_fd;
	conn_rec->file_fd = file_fd;
	conn_rec->file_size = stat_buf.st_size;
	conn_rec->remaining = stat_buf.st_size;
	conn_rec->io_buffer = (char*)malloc(IOBLOCK_SIZE);
	conn_rec->cstat = CONN_IDLE;
	if (!conn_rec->io_buffer) {
		/* Could not allocate a buffer. Free the conn_rec. */
		conn_rec->conn_fd = -1;
		return 0;
	}

	return conn_rec;
}

static int setupUring(struct io_uring* uring)
{
	int rc;
	rc = io_uring_queue_init(URING_DEPTH,uring,0);
	if (rc < 0) {
		perror("setup uring");
		return -1;
	}
	return rc;
}

static void queue_read(struct connection_rec *crec)
{

}

static void processConnection(struct connection_rec *crec) {
	if (crec->cstat == CONN_IDLE) {
		/* This is a new connection. We must queue a file read. */
		queue_read(crec);
	}
}

int main(int argc,char *argv[])
{
	int port,sock,rc;
	struct sockaddr_in addr,peer;
	fd_set connect_fds;
	const int BACKLOG = 8;
	int did_something;
	struct timeval timeout;

	if (argc < 2) {
		usage();
		exit(0);
	}

	initConnections();

	rc = setupUring(&uring);
	if (rc < 0) {
		perror("set up io_uring");
		exit(errno);
	}

	port = atoi(argv[1]);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(0);

	sock = socket(AF_INET,SOCK_STREAM,0);
	if (sock < 0) {
		perror("creating server socket");
		exit(errno);
	}

	rc = bind(sock,(struct sockaddr*)&addr,sizeof(addr));
	if (rc < 0) {
		perror("binding socket");
		exit(errno);
	}

	rc = listen(sock,BACKLOG);

	FD_SET(sock,&connect_fds);
	did_something = 0;
	timeout.tv_sec = 0;
	while (1) {
		int conn_fd;
		socklen_t peerlen = sizeof(peer);
		struct connection_rec* conn_rec;
		int conn_idx;
		if (did_something) {
			timeout.tv_usec = 0;
		} else {
			timeout.tv_usec = 10;
		}
		rc = select(sock+1,&connect_fds,0,0,&timeout);
		if (rc < 0) {
			perror("select");
			exit(errno);
		}
		if (rc > 0) {
			conn_fd = accept(sock,(struct sockaddr*)&peer,&peerlen);
			if (conn_fd < 0) {
				perror("accept");
				exit(errno);
			}

			/* We have a new connection. Start handling it. */
			conn_rec = buildConnectionRecord(conn_fd);
			if (!conn_rec) {
				/* Too many connections. */
				write(conn_fd,(void*)"Too many connections",20);
				close(conn_fd);
			}
			
			did_something = 1;
		}

		/* Check each in-use connection_rec and see if we need
		   to queue up an I/O operation. */
		for (conn_idx=0; conn_idx<sizeof(connections)/sizeof(connections[0]); ++conn_idx) {
			if (connections[conn_idx].conn_fd > 0) {
				/* This connection is in use. Process it. */
				processConnection(&connections[conn_idx]);
			}
		}
	}

	close(sock);

	return 0;
}
