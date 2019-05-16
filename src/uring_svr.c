/*
 * uring_svr.c
 *
 * vim: shiftwidth=4 tabstop=4 softtabstop=4 noexpandtab
 *
 * This file implements a very simple file server using the
 * Linux io_uring acynchronous I/O API.
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

/* Keep track of the data associated with a file being served. */
struct connection_rec {
	int rec_id;
	int socket_fd;
	int file_fd;
	int uring_fd;
	char* read_buffer;
	char* write_buffer;
};

static void usage()
{
	puts("Usage: uring_svr <port>");
}

int main(int argc,char *argv[])
{
	int port,sock,rc;
	struct sockaddr_in addr;

	if (argc < 2) {
		usage();
		exit(0);
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

	return 0;
}
