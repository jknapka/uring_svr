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
 * named regular file O_RDONLY and serve its contents to the
 * client. When the entire file contents are sent, it closes
 * the client connection. If the file does not exist, an error
 * message is written to the client and the connection is
 * closed immediately.
 *
 * Note that this is NOT an HTTP server, it uses its own
 * simple file retrieval protocol. To retrieve a file from
 * the server:
 *
 *  echo <filename> | nc <server.address> <port>
 *
 * The given <filename> must exist *on the server* in a place
 * accessible to the server process. It can be specified as
 * an absolute path or as a path relative to the uring_svr
 * process's working directory.
 *
 * Implementation notes:
 *
 * - You must be running kernel 5.1.0 or higher for this
 *   code to work.
 *
 * - The io_uring API is described here:
 *     http://kernel.dk/io_uring.pdf
 *   However, the API has some small differences from what
 *   is described in that document (possibly due to typographical
 *   errors).
 *
 * - This program uses the liburing wrapper around the
 *   kernel API. The best reference to that wrapper is
 *   the example code in the liburing repository, which
 *   can be found here:
 *     http://git.kernel.dk/cgit/liburing
 *   You must build liburing.so.1.0.1 and possibly fix
 *   its path name in the Makefile in order for this
 *   code to link successfully.
 *
 * - Up to MAX_CONNECTIONS connections are supported.
 *
 * - We use a single kernel io_uring for all async I/O requests.
 *
 * - We use a single thread to handle all I/O activity.
 *
 * - We allow at most one I/O request to be outstanding
 *   for any given connection, either a read or a write.
 *   We read a block into a buffer from a file, then write
 *   that same buffer to the connected socket. This is the
 *   main inefficiency in this code, but it makes the
 *   bookkeeping dead easy. A future version may
 *   do something smarter.
 *
 * - Remarkably, after fixing compilation errors, this code
 *   worked correctly the second time I ran it. The only real
 *   issue was getting the placement of io_uring_sqe_set_data()
 *   right.
 *
 * Copyright (c) 2019 Joseph A. Knapka. All rights reserved
 * under the provisions of the MIT license.
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
#define CONN_IDLE 0          /* The connection is active but no I/O has been queued. */
#define CONN_READ 1          /* Waiting for a read to complete. */
#define CONN_WRITE 2         /* Waiting for a write to complete. */
#define CONN_NEED_REQUEUE 4  /* An I/O operation failed and needs to be retried.
							    (Or'd with other values.)*/

/* Size of I/O buffers. */
static const int IOBLOCK_SIZE = 1024;

/* How deep the uring should be */
static const int URING_DEPTH = 128;

/* Maximum number of simultaneous connections we support. */
#define MAX_CONNECTIONS 128

/* Size of the buffer into which we read filenames from clients. */
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

/* Counts the number of active connections. */
int countConnections()
{
	int ii,count=0;
	for (ii=0; ii<MAX_CONNECTIONS; ++ii) {
		count += (connections[ii].conn_fd > 0);
	}
	return count;
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

	if (verbose >= 4) printf("Read filename: %s %ld\n",fname,strlen(fname));

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
		close(conn_fd);
		return 0;
	}

	/* Get the file size. */
	rc = fstat(file_fd,&stat_buf);
	if (rc < 0) {
		rc = write(conn_fd,"Could not stat file",19);
		perror("get file size");
		close(file_fd);
		close(conn_fd);
		return 0;
	}

	/* Find an available connection_rec. */
	conn_rec = getConnectionRecord();
	if (!conn_rec) {
		rc = write(conn_fd,"Too many connections",20);
		close(file_fd);
		close(conn_fd);
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
		close(file_fd);
		close(conn_fd);
		conn_rec->conn_fd = -1;
		return 0;
	}

	return conn_rec;
}

/* Initialize an io_uring instance for async I/O. */
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

	if (verbose >= 5) printf("queue_read @offset %ld with %ld remaining\n",crec->offset,crec->remaining);
	return 0;
}

/* Queue a socket write of the last data read into a connection_rec.
   This is extremely similar to queue_read(). */
static int queue_write(struct connection_rec *crec)
{
	/* Obtain a submission-queue entry from the kernel, if possible. */
	struct io_uring_sqe *sqe = io_uring_get_sqe(&uring);
	if (!sqe) {
		return -1;
	}

	/* Set up the connection_rec for a write of its io_buffer. */
	crec->cstat = CONN_WRITE;
	crec->iov.iov_base = crec->io_buffer;
	crec->iov.iov_len = crec->last_read; /* Write only the amount of data we last read. */

	/* io_uring submission logic:
	   Prepare the write. */
	io_uring_prep_writev(sqe,crec->conn_fd,&crec->iov,1,0);

	/* Set the data pointer so we can retrieve the connection_rec
	   upon completion. Again, this must be called AFTER
	   io_uring_prep_writev(), since the prep operation resets
	   the SQE's data pointer. */
	io_uring_sqe_set_data(sqe,crec);

	/* Submit the SQE to the kernel. */
	io_uring_submit(&uring);

	if (verbose >= 5) printf("queue_write @offset %ld with %ld remaining\n",crec->offset,crec->remaining);
	return 0;
}

/* Re-queue a read or write that could not be completed
   by the kernel. Any adjustments to the data being
   read or written are handled by the caller. 
  
   cqe is the completion that ended unexpectedly.
   crec is the connection_rec associated with the request.
   errcode is the error code of the completion. */
static int requeue(struct io_uring_cqe* cqe,struct connection_rec* crec,int errcode)
{
	/* Objtain a new sqe. */
	struct io_uring_sqe *sqe = io_uring_get_sqe(&uring);
	if (!sqe) {
		/* We need to requeue this again after a while. */
		crec->cstat |= CONN_NEED_REQUEUE;
		return -1;
	}
	if (crec->cstat == CONN_WRITE) {
		/* This was an incomplete write, so prep a new write. */
		io_uring_prep_writev(sqe,crec->conn_fd,&crec->iov,1,0);
	} else {
		/* This was an incomplate read, so prep a new read. */
		io_uring_prep_readv(sqe,crec->file_fd,&crec->iov,1,crec->offset);
	}

	/* Set the data pointer so we can retrieve the connection_rec later.
	   As in queue_read() and queue_write(), this must be done after
	   io_uring_prep_*(). */
	io_uring_sqe_set_data(sqe,crec);

	/* Submit the new SQE. */
	io_uring_submit(&uring);

	return 0;
}

/* When a connection's file has been fully sent to the client,
   close down the connection and the file. */
static void shutdownConnection(struct connection_rec *crec)
{
	int rc;
	rc = close(crec->conn_fd);
	if (verbose >= 3) printf("closed connection, rc=%d\n",rc);
	rc = close(crec->file_fd);
	if (verbose >= 3) printf("closed file, rc=%d\n",rc);
	crec->conn_fd = -1; /* Frees the connection_rec. */
	if (verbose) printf("-Now serving %d connections.\n",countConnections());
}

/* If a connection_rec requires any action that is not
   handled during io_uring completion processing, do
   that here. */
static int processConnection(struct connection_rec *crec)
{
	if (crec->cstat == CONN_IDLE) {
		/* This is a new connection. We must queue a file read. */
		queue_read(crec);
	} else if (crec->cstat & CONN_NEED_REQUEUE) {
		/* A previous I/O operation completed erroneously and
		   could not be requeued at the time. Try to requeue again. */
		crec->cstat &= ~CONN_NEED_REQUEUE;
		requeue(0,crec,0);
	} else if (crec->remaining == 0) {
		/* We have successfully read the entire file. */
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

/* Handle the completion of an async I/O operation. */
static void processCompletion(struct io_uring_cqe *cqe)
{
	/* Retrieve the connection_req from the completion queue entry. */
	struct connection_rec *crec = (struct connection_rec*)io_uring_cqe_get_data(cqe);

	if (cqe->res < 0) {
		/* An error occurred. */
		if (cqe->res == -EAGAIN) {
			/* The operation failed. Requeue the entire thing. */
			if (verbose >= 2) printf("requeue (EAGAIN) @offset %ld\n",crec->offset);
			requeue(cqe,crec,-EAGAIN);
		} else {
			/* We're not sure what happened here... probably best
			 to shut down the connection. */
			perror("processing completion");
			printf("cqe->res == %d, EAGAIN == %d\n",cqe->res,EAGAIN);
			shutdownConnection(crec);
		}
	} else {
		if (cqe->res < crec->iov.iov_len) {
			/* Short read or write. Adjust the data pointer and
			 length and requeue. */
			crec->remaining -= cqe->res;
			crec->iov.iov_len -= cqe->res;
			crec->iov.iov_base += cqe->res;
			if (crec->cstat == CONN_READ) {
				crec->offset += cqe->res;
			}
			if (verbose >= 2) printf("requeue (short %d) @offset %ld\n",cqe->res,crec->offset);
			requeue(cqe,crec,0);
		} else {
			/* The entire I/O operation completed successfully. We have
			   the complete crec->io_buffer full of data. */
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
		}
	}
	/* Tell the kernel we are done with the CQE. */
	io_uring_cqe_seen(&uring,cqe);
}

/* Becomes true when ^C is pressed. */
static int stopService = 0;

/* Handle ^C by setting the "stop" flag. */
void sigint_handler(int sig)
{
	stopService = 1;
}

/* Create a socket and bind it to port as a listener. */
static int setupListenSocket(int port)
{
	struct sockaddr_in addr;
	int rc,sock;
	const int one = 1;
	const int BACKLOG = 8;

	/* Configure the address to bind. We listen on all interfaces. */
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(0);

	/* Create the server socket. */
	sock = socket(AF_INET,SOCK_STREAM,0);
	if (sock < 0) {
		perror("creating server socket");
		return errno;
	}

	/* Re-use the port, so we can re-bind after exiting. */
	rc = setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
	if (rc < 0) {
		perror("re-using address");
		return errno;
	}

	/* Bind to the port. */
	rc = bind(sock,(struct sockaddr*)&addr,sizeof(addr));
	if (rc < 0) {
		perror("binding socket");
		return errno;
	}

	/* Start listening for connections. */
	rc = listen(sock,BACKLOG);

	return sock;
}

/* Count the number of 'v' characters in a string. */
static int countVs(char const *s)
{
	int cc;
	for (cc=0; *s; ++s) {
		if (*s == 'v') ++cc;
	}
	return cc;
}

/* Parse the command-line arguments. There are only two
   possible arguments:

   1) The port to listen on (mandatory).
   2) -v[vv..] - verbosity level (optional). More v's == more output.
   */
static int parseArgs(int argc, char *argv[])
{
	int ii;
	int port = atoi(argv[1]);
	if (argc < 2) {
		usage();
		exit(0);
	}

	for (ii=2; ii<argc; ++ii) {
		if (!strncmp(argv[ii],"-v",2)) {
			verbose = countVs(argv[ii]);
		}
	}

	return port;
}

int main(int argc,char *argv[])
{
	int port,rc,ii;
	struct sockaddr_in peer;
	fd_set connect_fds;
	int did_something;
	struct timeval timeout;
	int one = 1;
	char fname[FNAME_SZ];
	int idle_loops = 0;

	port = parseArgs(argc,argv);
	if (port <= 0) {
		usage();
		exit(1);
	}

	signal(SIGINT,sigint_handler);

	initConnections();

	rc = setupUring(&uring);
	if (rc < 0) {
		perror("set up io_uring");
		exit(errno);
	}

	sock = setupListenSocket(port);
	if (sock < 0) {
		exit(errno);
	}

	/* If we took any I/O action during a given trip through
	   the processing loop, this becomes true and we select()
	   with 0 timeout, anticipating some immediate additional
	   activity. Otherwise we select() with a 10μs timeout. */
	did_something = 0;

	/* Set up the select() timeout. */
	timeout.tv_sec = 0;

	/* The connection processing loop. */
	while (!stopService) {
		int conn_fd;
		socklen_t peerlen = sizeof(peer);
		struct connection_rec* conn_rec;
		int conn_idx;

		if (did_something) {
			if (verbose > 5) printf("I/O after %d idle loops\n",idle_loops);
			timeout.tv_usec = 0;
			idle_loops = 0;
		} else {
			if (verbose > 10) printf("no active I/O, select for 10μs\n");
			++idle_loops;
			timeout.tv_usec = 10;
		}
		did_something = 0;
		
		/* See if there is a new connection to handle. */
		FD_SET(sock,&connect_fds);
		rc = select(sock+1,&connect_fds,0,0,&timeout);
		if (rc < 0) {
			perror("select");
			exit(errno);
		}

		if (rc > 0) {
			/* Accept the new connection. */
			conn_fd = accept(sock,(struct sockaddr*)&peer,&peerlen);
			if (conn_fd < 0) {
				perror("accept");
				exit(errno);
			}

			/* We have a new connection. Start handling it. We read
			 a filename from the client using a regular read() call.
			 Once we have the filename, all further I/O is handled
			 via io_uring. */
			rc = readFname(conn_fd,fname,FNAME_SZ);
			if (rc == 0) {
				conn_rec = buildConnectionRecord(conn_fd,fname);
				if (!conn_rec) {
					/* Could not set up connection. */
					close(conn_fd);
				}
			}

			if (verbose) printf("+Now serving %d connections.\n",countConnections());
			fflush(stdout);

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
			for (rc = io_uring_peek_cqe(&uring,&cqe);
					rc == 0 && cqe;
					rc = io_uring_peek_cqe(&uring,&cqe)) {
				did_something = 1;
				processCompletion(cqe);
			}
		}
	}

	close(sock);

	return 0;
}
