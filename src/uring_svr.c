/*
 * uring_svr.c
 *
 * vim: shiftwidth=4 tabstop=4 softtabstop=4 noexpandtab
 *
 * This file implements a very simple file server using the
 * Linux io_uring acynchronous I/O API. It does not make
 * especially efficient use of the io_uring mechanism at this
 * point. I am only trying to come to grips with the API.
 *
 * Usage:
 *
 *  ./uring_svr <port> [-v]
 *
 * The server will listen for TCP/IP connections on <port>.
 * When one is received, the server reads a newline-terminated
 * filename from the client. It then attempts to open the
 * named file O_RDONLY and serve its contents to the client.
 * When the entire file contents are sent, it closes the
 * client connection. If the file does not exist, an error
 * message is written to the client and the connection is
 * closed immediately.
 *
 * Implementation notes:
 *
 * - You must be running kernel 5.1.0 or higher for this
 *   code to work.
 *
 * - The io_uring API is described here:
 *     http://kernel.dk/io_uring.pdf
 *   However, the API has some small differences from what
 *   is described in that document.
 *
 * - This program uses the liburing wrapper around the
 *   kernel API. The best reference to that wrapper is
 *   the example code in the liburing repository, which
 *   can be found here:
 *     http://git.kernel.dk/cgit/liburing
 *
 * - Up to MAX_CONNECTIONS connections are supported.
 *
 * - We use a single kernel io_uring for all async I/O requests.
 *
 * - We allow at most one I/O request to be outstanding
 *   for any given connection, either a read or a write.
 *   We read a block into a buffer from a file, then write
 *   that same block to the connected socket. This is the
 *   main inefficiency in this code, but it makes the
 *   bookkeeping dead easy. A future version will
 *   do something smarter.
 *
 * - Remarkably, this code worked correctly the second
 *   time I ran it. The only real issue was getting the
 *   placement of io_uring_sqe_set_data() right.
 *
 * Copyright (c) 2019 Joseph A. Knapka.
 *
 * LICENSE: MIT
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
#include <signal.h>
#include <io_uring.h>
#include <liburing.h>

/* If true, print some info while serving files. */
int verbose = 0;

/* Keep track of the data associated with a file being served. */
struct connection_rec {
	int conn_fd;       /* The fd of the client socket. */
	int file_fd;       /* The fd of the file being served. */
	size_t remaining;  /* The amount of data left to be read from the file. */
	off_t offset;      /* The next offset to read from the file. */
	int last_read;     /* The size of the last buffer read from the file. */
	int cstat;         /* 0 = idle, 1 = waiting for read, 2 = waiting for write. */
	struct iovec iov;  /* iovec to give to the uring API. */
	char* io_buffer;   /* Buffer for reads and writes for this connection and file. */
};

/* States of connection_rec->cstat. */
#define CONN_IDLE 0
#define CONN_READ 1
#define CONN_WRITE 2

/* Size of I/O buffers. */
static const int IOBLOCK_SIZE = 1024;

/* How deep the uring should be */
static const int URING_DEPTH = 32;

/* Maximum number of simultaneous connections we support. */
#define MAX_CONNECTIONS 16

static const int FNAME_SZ = 1024;

/* All connection records. */
static struct connection_rec connections[MAX_CONNECTIONS];

/* Ths uring instance itself. */
struct io_uring uring;

/* The server socket. */
int sock = 0;

/* Print a simple usage message. */
static void usage()
{
	puts("Usage: uring_svr <port> [-v]");
}

/* Do basic initialization of all connection_recs. */
static void initConnections()
{
	int ii;
	for (ii=0; ii<MAX_CONNECTIONS; ++ii) {
		/* A conn_fd of -1 indicates an unused connection. */
		connections[ii].conn_fd = -1;
	}
}

/* Allocate a connection record if one is available. */
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

/* Read a newline-terminated filename from the given fd into
   the given fname buffer of length len. */
static int readFname(int fd,char *fname,size_t len)
{
	int rc, got_nl;
	char *fend, *rd_at, *nl;

	rd_at = fname;
	fend = fname+len-1;
	nl = 0;

	memset(fname,0,len);

	/* Keep reading until we fill the buffer or see a newline. */
	while (!nl && (rd_at < fend)) {
		len = fend - rd_at;
		rc = read(fd,(void*)rd_at,len);
		if (rc < 0) {
			perror("reading filename");
			return errno;
		}
		if (nl = strchr(rd_at,'\n')) {
			*nl = 0;
		} else {
			rd_at += rc;
		}
	}

	if (verbose) printf("Read filename: %s %ld\n",fname,strlen(fname));

	return 0;
}

/* Initialize the connection_rec to serve a given filename
   on a connected socket. Return a pointer to the initialized
   connection_rec, or NULL if initialization fails. */
static struct connection_rec* buildConnectionRecord(int conn_fd,char* fname)
{
	int rc, file_fd, uring_fd;
	struct stat stat_buf;
	struct connection_rec* conn_rec;

	/* Open the requested file. */
	file_fd = open(fname,O_RDONLY);
	if (file_fd < 0) {
		rc = write(conn_fd,"Could not open file",19);
		perror("opening file");
		return 0;
	}

	/* Get the file size. */
	rc = fstat(file_fd,&stat_buf);
	if (rc < 0) {
		rc = write(conn_fd,"Could not stat file",19);
		perror("get file size");
		return 0;
	}

	/* Find an available connection_rec. */
	conn_rec = getConnectionRecord();
	if (!conn_rec) {
		rc = write(conn_fd,"Too many connections",20);
		return 0;
	}

	/* Initialize... */
	conn_rec->conn_fd = conn_fd;
	conn_rec->file_fd = file_fd;
	conn_rec->remaining = stat_buf.st_size;
	conn_rec->last_read = 0;
	conn_rec->offset = 0;
	conn_rec->io_buffer = (char*)malloc(IOBLOCK_SIZE);
	conn_rec->cstat = CONN_IDLE;
	if (!conn_rec->io_buffer) {
		/* Could not allocate a buffer. Free the conn_rec. */
		rc = write(conn_fd,"Memory failure",14);
		conn_rec->conn_fd = -1;
		return 0;
	}

	return conn_rec;
}

/* Initialize an ui_uring instance for async I/O. */
static int setupUring(struct io_uring* uring)
{
	int rc;

	/* This is a wrapper supplied by liburing. It allocates
	   a io_uring in the kernel of the given depth, with no
	   special characteristics (eg no kernel polling, etc.). */
	rc = io_uring_queue_init(URING_DEPTH,uring,0);
	if (rc < 0) {
		perror("setup uring");
		return rc;
	}
	return rc;
}

/* Queue an async read into the kernel io_uring. */
static int queue_read(struct connection_rec *crec)
{
	/* Get a uring submission-queue entry. */
	struct io_uring_sqe* sqe = io_uring_get_sqe(&uring);
	if (!sqe) {
		return -1;
	}

	/* Set up the connection_rec state to reflect a new read. */
	crec->cstat = CONN_READ;
	crec->iov.iov_base = crec->io_buffer; /* Set up the iovec the kernel will see. */
	crec->iov.iov_len = IOBLOCK_SIZE;
	crec->last_read = IOBLOCK_SIZE;
	if (crec->remaining < IOBLOCK_SIZE) {
		/* Set up a short read if there's not a full block left in the file. */
		crec->iov.iov_len = crec->last_read = crec->remaining;
	}

	/* Set up the submission to the uring. These are wrappers provided
	   by liburing. */

	/* Set up an async read of an iovec (just one, in this case, but in
	   general it could be more). */
	io_uring_prep_readv(
			sqe,           /* The SQE being prepped. */
			crec->file_fd, /* The fd for the async I/O to operate on. */
			&crec->iov,    /* The iovec array and count to read into. */
			1,
			crec->offset); /* The file offset to start reading at. */

	/* Associate this connection_rec with the sqe so that we can retrieve it
	   when handling the completion of this I/O operation.
	   TRAP FOR THE UNWARY: this must be called AFTER io_uring_prep_readv(),
	   becaue the prep operation resets the data pointer in the SQE. */
	io_uring_sqe_set_data(sqe,crec);

	/* Actually submit the SQE to the kernel io_uring. This submits
	   all SQEs obtained via io_uring_get_sqe() since the last
	   call to io_uring_submit(), which in this case is just one.
	 */
	io_uring_submit(&uring);
	/* At this point the operation is submitted and the kernel
	   will notify us via the completion queue when the operation
	   completes. */

	if (verbose) printf("queue_read @offset %ld with %ld remaining\n",crec->offset,crec->remaining);
	return 0;
}

/* Queue a socket write of the last data read into a connection_rec.
   This is extremely similar to queue_read(). */
static int queue_write(struct connection_rec *crec)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&uring);
	if (!sqe) {
		return -1;
	}
	crec->cstat = CONN_WRITE;
	crec->iov.iov_base = crec->io_buffer;
	crec->iov.iov_len = crec->last_read;
	io_uring_prep_writev(sqe,crec->conn_fd,&crec->iov,1,0);
	io_uring_sqe_set_data(sqe,crec);
	io_uring_submit(&uring);
	if (verbose) printf("queue_write @offset %ld with %ld remaining\n",crec->offset,crec->remaining);
	return 0;
}

static void shutdownConnection(struct connection_rec *crec)
{
	int rc;
	rc = close(crec->conn_fd);
	if (verbose) printf("closed connection, rc=%d\n",rc);
	rc = close(crec->file_fd);
	if (verbose) printf("closed file, rc=%d\n",rc);
	crec->conn_fd = -1; /* Frees the connection_rec. */
}

static int processConnection(struct connection_rec *crec)
{
	if (crec->cstat == CONN_IDLE) {
		/* This is a new connection. We must queue a file read. */
		queue_read(crec);
		return 1;
	} else if (crec->remaining == 0) {
		if (crec->cstat == CONN_READ) {
			/* Last read complete. Queue the corresponding write. */
			queue_write(crec);
		} else {
			/* This connection is complete and can be shut down. */
			shutdownConnection(crec);
		}
	}
	/* All other cases are handled during uring completion processing. */
	return 0;
}

static int requeue(struct io_uring_cqe* cqe,struct connection_rec* crec,int errcode)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&uring);
	if (!sqe) {
		return -1;
	}
	if (crec->cstat == CONN_WRITE) {
		io_uring_prep_writev(sqe,crec->conn_fd,&crec->iov,1,0);
	} else {
		io_uring_prep_readv(sqe,crec->file_fd,&crec->iov,1,crec->offset);
	}
	io_uring_sqe_set_data(sqe,crec);
	io_uring_submit(&uring);
	return 0;
}

static void processCompletion(struct io_uring_cqe *cqe)
{
	struct connection_rec *crec = (struct connection_rec*)io_uring_cqe_get_data(cqe);
	if (cqe->res < 0) {
		if (cqe->res == -EAGAIN) {
			if (verbose) printf("requeue (EAGAIN) @offset %ld\n",crec->offset);
			requeue(cqe,crec,-EAGAIN);
		} else {
			perror("processing completion");
		}
		io_uring_cqe_seen(&uring,cqe);
		return;
	} else {
		if (cqe->res < crec->iov.iov_len) {
			/* Short read or write. */
			crec->remaining -= cqe->res;
			crec->iov.iov_len -= cqe->res;
			crec->iov.iov_base += cqe->res;
			if (crec->cstat == CONN_READ) {
				crec->offset += cqe->res;
			}
			if (verbose) printf("requeue (short %d) @offset %ld\n",cqe->res,crec->offset);
			requeue(cqe,crec,0);
			io_uring_cqe_seen(&uring,cqe);
		} else {
			if (crec->cstat == CONN_READ) {
				/* Read complete. Update offset and then queue the
				   corresponding write. */
				crec->offset += cqe->res;
				crec->remaining -= cqe->res;
				queue_write(crec);
			} else {
				/* Write complete. Queue the next read. */
				if (crec->remaining > 0) {
					queue_read(crec);
				}
			}
			io_uring_cqe_seen(&uring,cqe);
		}
	}
}

void sigint_handler(int sig)
{
	if (sock > 0) {
		close(sock);
		puts("closed socket");
	}
}

int main(int argc,char *argv[])
{
	int port,rc,ii;
	struct sockaddr_in addr,peer;
	fd_set connect_fds;
	const int BACKLOG = 8;
	int did_something;
	struct timeval timeout;
	int one = 1;
	char fname[FNAME_SZ];

	if (argc < 2) {
		usage();
		exit(0);
	}

	for (ii=1; ii<argc; ++ii) {
		if (!strncmp(argv[ii],"-v",2)) {
			verbose = 1;
		}
	}

	signal(SIGINT,sigint_handler);

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

	rc = setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
	if (rc < 0) {
		perror("re-using address");
		exit(errno);
	}

	rc = bind(sock,(struct sockaddr*)&addr,sizeof(addr));
	if (rc < 0) {
		perror("binding socket");
		exit(errno);
	}

	rc = listen(sock,BACKLOG);

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
		FD_SET(sock,&connect_fds);
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
			rc = readFname(conn_fd,fname,FNAME_SZ);
			if (rc == 0) {
				conn_rec = buildConnectionRecord(conn_fd,fname);
				if (!conn_rec) {
					/* Could not set up connection. */
					close(conn_fd);
				}
			}
			
			did_something = 1;
		}

		/* Check each in-use connection_rec and see if we need
		   to queue up an I/O operation. */
		for (conn_idx=0; conn_idx<sizeof(connections)/sizeof(connections[0]); ++conn_idx) {
			if (connections[conn_idx].conn_fd > 0) {
				/* This connection is in use. Process it. */
				if (processConnection(&connections[conn_idx])) {
					did_something = 1;
				}
			}
		}

		/* See if there are uring completions to process. */
		{
			struct io_uring_cqe* cqe;
			rc = io_uring_peek_cqe(&uring,&cqe);
			while (rc == 0 && cqe) {
				did_something = 1;
				processCompletion(cqe);
				rc = io_uring_peek_cqe(&uring,&cqe);
			}
		}
	}

	close(sock);

	return 0;
}
