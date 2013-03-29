/*
 * Copyright (C) 2013  Internet Systems Consortium, Inc. ("ISC")
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* DNS defines */

#define NS_TYPE_A		1
#define NS_TYPE_NS		2
#define NS_TYPE_CNAME		5
#define NS_TYPE_SOA		6
#define NS_TYPE_NULL		10
#define NS_TYPE_PTR		12
#define NS_TYPE_MX		15
#define NS_TYPE_TXT		16
#define NS_TYPE_AAAA		28
#define NS_TYPE_OPT		41
#define NS_TYPE_DS		43
#define NS_TYPE_RRSIG		46
#define NS_TYPE_NSEC		47
#define NS_TYPE_DNSKEY		48
#define NS_TYPE_NSEC3		50
#define NS_TYPE_NSEC3PARAM	51
#define NS_TYPE_TSIG		250
#define NS_TYPE_IXFR		251
#define NS_TYPE_AXFR		252
#define NS_TYPE_ANY		255

#define NS_CLASS_IN		1
#define NS_CLASS_ANY		255

#define NS_OFF_ID		0
#define NS_OFF_FLAGS		2
#define NS_OFF_QDCOUNT		4
#define NS_OFF_ANCOUNT		6
#define NS_OFF_NSCOUNT		8
#define NS_OFF_ARCOUNT		10
#define NS_OFF_QUESTION		12

#define NS_FLAG_QR		0x8000U
#define NS_FLAG_AA		0x0400U
#define NS_FLAG_TC		0x0200U
#define NS_FLAG_RD		0x0100U
#define NS_FLAG_RA		0x0080U
#define NS_FLAG_AD		0x0020U
#define NS_FLAG_CD		0x0010U

#define NS_XFLAG_DO		0x8000U

#define NS_OPCODE_MASK		0x7000U
#define NS_OPCODE_QUERY		0

#define NS_RCODE_MASK		0x000fU
#define NS_RCODE_NOERROR	0
#define NS_RCODE_FORMERR	1
#define NS_RCODE_SERVFAIL	2
#define NS_RCODE_NXDOMAIN	3
#define NS_RCODE_NOIMP		4
#define NS_RCODE_REFUSED	5
#define NS_RCODE_LAST		6

/* chaining macros */

#define ISC_INIT(head, headl)		do { \
	(head) = -1; \
	(headl) = &(head); \
} while (0)

#define ISC_INSERT(head, headl, elm)	do { \
	(elm)->next = -1; \
	(elm)->prev = (headl); \
	*(headl) = (elm) - xlist; \
	(headl) = &((elm)->next); \
} while (0)

#define ISC_REMOVE(headl, elm)		do { \
	if ((elm)->next != -1) \
		xlist[(elm)->next].prev = (elm)->prev; \
	else \
		(headl) = (elm)->prev; \
	*(elm)->prev = (elm)->next; \
} while (0)

/*
 * Data structures
 */

/*
 * exchange:
 *    - per exchange values:
 *	* order (for debugging)
 *	* id
 *	* random (for debugging)
 *	* time-stamps
 *
 * sent/rcvd chain, "next to be received" on entry cache.
 */

struct exchange {				/* per exchange structure */
	int sock;				/* socket descriptor */
	int next, *prev;			/* chaining */
#define X_FREE	0
#define X_CONN	1
#define X_READY	2
#define X_SENT	3
	int state;				/* state */
	uint16_t id;				/* ID */
	uint64_t order;				/* number of this exchange */
	struct timespec ts0, ts1, ts2, ts3;	/* timespecs */
};
struct exchange *xlist;				/* exchange list */
int xlast;					/* number of exchanges */
int xconn, *xconnl;				/* connecting list */
int xready, *xreadyl;				/* connected list */
int xsent, *xsentl;				/* sent list */
int xfree, *xfreel;				/* free list */
int xused;					/* next to be used list */
uint64_t xccount;				/* connected counters */
uint64_t xscount;				/* sent counters */
uint64_t xrcount;				/* received counters */

/*
 * statictics counters and accumulators
 */

uint64_t recverr, tooshort, locallimit;		/* error counters */
uint64_t loops, lateconn, compconn, shortwait;	/* rate stats */
uint64_t badconn, collconn, badsent, collsent;	/* rate stats (cont) */
uint64_t badid, notresp;			/* bad response counters */
uint64_t rcodes[NS_RCODE_LAST + 1];		/* rcode counters */
double dmin = 999999999.;			/* minimum delay */
double dmax = 0.;				/* maximum delay */
double dsum = 0.;				/* delay sum */
double dsumsq = 0.;				/* square delay sum */

/*
 * command line parameters
 */

int edns0;				/* EDNS0 DO flag */
int ipversion = 0;			/* IP version */
int rate;				/* rate in connections per second */
int report;				/* delay between two reports */
uint32_t range;				/* randomization range */
uint32_t maxrandom;			/* maximum random value */
int basecnt;				/* base count */
char *base[2];				/* bases */
int numreq;				/* number of exchange */
int period;				/* test period */
double losttime = 1.;			/* time after a request is lost  */
int maxdrop;				/* maximum number of lost requests */
double maxpdrop = 0.;			/* maximum percentage */
char *localname;			/* local address or interface */
int preload;				/* preload exchanges */
int aggressivity = 1;			/* back to back exchanges */
int seeded;				/* is a seed provided */
unsigned int seed;			/* randomization seed */
char *templatefile;			/* template file name */
int rndoffset = -1;			/* template offset (random) */
char *diags;				/* diagnostic selectors */
char *servername;			/* server */
int ixann;				/* ixann NXDOMAIN */

/*
 * global variables
 */

int locbind;
struct sockaddr_storage localaddr;	/* local socket address */
struct sockaddr_storage serveraddr;	/* server socket address */

int epoll_fd;				/* epoll file descriptor */
#ifndef EVENTS_CNT
#define EVENTS_CNT	16
#endif
struct epoll_event events[EVENTS_CNT];	/* polled events */
int interrupted, fatal;			/* to finish flags */

uint8_t obuf[4098], ibuf[4098];		/* I/O buffers */
char tbuf[4098];			/* template buffer */

struct timespec boot;			/* the date of boot */
struct timespec last;			/* the date of last connect */
struct timespec due;			/* the date of next connect */
struct timespec dreport;		/* the date of next reporting */
struct timespec finished;		/* the date of finish */

/*
 * template
 */

size_t length_query;
uint8_t template_query[4096];
size_t random_query;

/*
 * initialize data structures handling exchanges
 */

void
inits(void)
{
	int idx;

	ISC_INIT(xconn, xconnl);
	ISC_INIT(xready, xreadyl);
	ISC_INIT(xsent, xsentl);
	ISC_INIT(xfree, xfreel);

	epoll_fd = epoll_create(EVENTS_CNT);
	if (epoll_fd < 0) {
		perror("epoll_create");
		exit(1);
	}

	xlist = (struct exchange *) malloc(xlast * sizeof(struct exchange));
	if (xlist == NULL) {
		perror("malloc(exchanges)");
		exit(1);
	}
	memset(xlist, 0, xlast * sizeof(struct exchange));

	for (idx = 0; idx < xlast; idx++)
		xlist[idx].sock = xlist[idx].next = -1;
}

/*
 * build a TCP DNS QUERY
 */

void
build_template_query(void)
{
	uint8_t *p = template_query;
	uint16_t v;

	/* flags */
	p += NS_OFF_FLAGS;
	v = NS_FLAG_RD;
	*p++ = v >> 8;
	*p++ = v & 0xff;
	/* qdcount */
	v = 1;
	*p++ = v >> 8;
	*p++ = v & 0xff;
	/* ancount */
	v = 0;
	*p++ = v >> 8;
	*p++ = v & 0xff;
	/* nscount */
	v = 0;
	*p++ = v >> 8;
	*p++ = v & 0xff;
	/* arcount */
	v = edns0;
	*p++ = v >> 8;
	*p++ = v & 0xff;
	/* icann.link (or ixann.link) */
	*p++ = 5;
	*p++ = 'i';
	if (ixann == 0)
		*p++ = 'c';
	else
		*p++ = 'x';
	*p++ = 'a';
	*p++ = 'n';
	*p++ = 'n';
	*p++ = 4;
	*p++ = 'l';
	*p++ = 'i';
	*p++ = 'n';
	*p++ = 'k';
	*p++ = 0;
	/* type A/AAAA */
	if (ipversion == 4)
		v = NS_TYPE_A;
	else
		v = NS_TYPE_AAAA;
	*p++ = v >> 8;
	*p++ = v & 0xff;
	/* class IN */
	v = NS_CLASS_IN;
	*p++ = v >> 8;
	*p++ = v & 0xff;
	/* EDNS0 OPT with DO */
	if (edns0) {
		/* root name */
		*p++ = 0;
		/* type OPT */
		v = NS_TYPE_OPT;
		*p++ = v >> 8;
		*p++ = v & 0xff;
		/* class UDP length */
		v = 4096;
		*p++ = v >> 8;
		*p++ = v & 0xff;
		/* extended rcode 0 */
		*p++ = 0;
		/* version 0 */
		*p++ = 0;
		/* extended flags DO */
		v = NS_XFLAG_DO;
		*p++ = v >> 8;
		*p++ = v & 0xff;
		/* rdlength */
		v = 0;
		*p++ = v >> 8;
		*p++ = v & 0xff;
	}
	/* length */
	length_query = p - template_query;
}

/*
 * get a TCP DNS client QUERY template
 * from the file given in the command line (-T<template-file>)
 * and rnd offset (-O<random-offset>)
 */

void
get_template_query(void)
{
	uint8_t *p = template_query;
	int fd, cc, i, j;

	fd = open(templatefile, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n",
			templatefile, strerror(errno));
		exit(2);
	}
	cc = read(fd, tbuf, sizeof(tbuf));
	(void) close(fd);
	if (cc < 0) {
		fprintf(stderr, "read(%s): %s\n",
			templatefile, strerror(errno));
		exit(1);
	}
	if (cc < NS_OFF_QUESTION + 6) {
		fprintf(stderr,"file '%s' too small\n", templatefile);
		exit(2);
	}
	if (cc > 4096) {
		fprintf(stderr,"file '%s' too large\n", templatefile);
		exit(2);
	}
	j = 0;
	for (i = 0; i < cc; i++) {
		if (isspace((int) tbuf[i]))
			continue;
		if (!isxdigit((int) tbuf[i])) {
			fprintf(stderr,
				"illegal char[%d]='%c' in file '%s'\n",
				i, (int) tbuf[i], templatefile);
			exit(2);
		}
		tbuf[j] = tbuf[i];
		j++;
	}
	cc = j;
	if ((cc & 1) != 0) {
		fprintf(stderr,
			"odd number of hexadecimal digits in file '%s'\n",
			templatefile);
		exit(2);
	}
	length_query = cc >> 1;
	for (i = 0; i < cc; i += 2)
		(void) sscanf(tbuf + i, "%02hhx", &p[i >> 1]);
	if (rndoffset >= 0)
		random_query = (size_t) rndoffset;
	if (random_query > length_query) {
		fprintf(stderr,
			"random (at %zu) outside the template (length %zu)?\n",
			random_query, length_query);
		exit(2);
	}
}

#if 0
/*
 * randomize the value of the given field:
 *   - offset of the field
 *   - random seed (used as it when suitable)
 *   - returns the random value which was used
 */

uint32_t
randomize(size_t offset, uint32_t r)
{
	uint32_t v;

	if (range == 0)
		return 0;
	if (range == UINT32_MAX)
		return r;
	if (maxrandom != 0)
		while (r >= maxrandom)
			r = (uint32_t) random();
	r %= range + 1;
	v = r;
	v += obuf[offset];
	obuf[offset] = v;
	if (v < 256)
		return r;
	v >>= 8;
	v += obuf[offset - 1];
	obuf[offset - 1] = v;
	if (v < 256)
		return r;
	v >>= 8;
	v += obuf[offset - 2];
	obuf[offset - 2] = v;
	if (v < 256)
		return r;
	v >>= 8;
	v += obuf[offset - 3];
	obuf[offset - 3] = v;
	return r;
}
#endif

/*
 * flush/timeout connect
 */

void
flushconnect(void)
{
	struct exchange *x;
	struct timespec now;
	int idx = xconn;
	int cnt = 10;
	double waited;

	if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
		perror("clock_gettime(flushconnect)");
		fatal = 1;
		return;
	}

	while (--cnt >= 0) {
		if (idx < 0)
			return;
		x = xlist + idx;
		idx = x->next;
		if (x->state != X_CONN)
			abort();
		/* check for a timed-out connection */
		waited = now.tv_sec - x->ts0.tv_sec;
		waited += (now.tv_nsec - x->ts0.tv_nsec) / 1e9;
		if (waited < losttime)
			return;
		/* garbage collect timed-out connections */
		ISC_REMOVE(xconnl, x);
		(void) close(x->sock);
		x->sock = -1;
		collconn++;
		x->state = X_FREE;
		ISC_INSERT(xfree, xfreel, x);
	}
}

/*
 * poll connected
 */

void
pollconnect(int topoll)
{
	struct exchange *x;
	int evn, idx, err;
	socklen_t len = sizeof(int);

	for (evn = 0; evn < topoll; evn++) {
		idx = events[evn].data.fd;
		x = xlist + idx;
		if (x->state != X_CONN)
			continue;
		if (events[evn].events == 0)
			continue;
		ISC_REMOVE(xconnl, x);
		events[evn].events = 0;
		if ((getsockopt(x->sock, SOL_SOCKET, SO_ERROR,
				&err, &len) < 0) ||
		    (err != 0)) {
			(void) close(x->sock);
			x->sock = -1;
			badconn++;
			x->state = X_FREE;
			ISC_INSERT(xfree, xfreel, x);
			continue;
		}
		x->state = X_READY;
		ISC_INSERT(xready, xreadyl, x);
	}
}

/*
 * send the TCP DNS QUERY
 */

int
sendquery(struct exchange *x)
{
	ssize_t ret;

	obuf[0] = length_query >> 8;
	obuf[1]= length_query & 0xff;
	memcpy(obuf + 2, template_query, length_query);
	/* ID */
	memcpy(obuf + 2 + NS_OFF_ID, &x->id, 2);
#if 0
	/* random */
	if (random_query > 0)
		x->rnd = randomize(random_query + 2, x->rnd);
#endif
	/* timestamp */
	errno = 0;
	ret = clock_gettime(CLOCK_REALTIME, &x->ts2);
	if (ret < 0) {
		perror("clock_gettime(send)");
		fatal = 1;
		return -errno;
	}
	ret = send(x->sock, obuf, length_query + 2, 0);
	if (ret == (ssize_t) length_query + 2)
		return 0;
	return -errno;
}	

/*
 * poll ready and send
 */

void
pollsend(void)
{
	struct exchange *x;
	int idx = xready;
	struct epoll_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
	for (;;) {
		if (idx < 0)
			return;
		x = xlist + idx;
		ev.data.fd = idx;
		idx = x->next;
		ISC_REMOVE(xreadyl, x);
		if (sendquery(x) < 0) {
			(void) close(x->sock);
			x->sock = -1;
			badsent++;
			x->state = X_FREE;
			ISC_INSERT(xfree, xfreel, x);
			continue;
		}
		xscount++;
		x->state = X_SENT;
		ISC_INSERT(xsent, xsentl, x);
		if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, x->sock, &ev) < 0) {
			perror("epoll_fd");
			fatal = 1;
			return;
		}
	}
}

/*
 * receive a TCP DNS RESPONSE
 */

void
receiveresp(struct exchange *x)
{
	struct timespec now;
	ssize_t cc;
	uint16_t v;
	double delta;

	cc = recv(x->sock, ibuf, sizeof(ibuf), 0);
	if (cc < 0) {
		if ((errno == EAGAIN) ||
		    (errno == EWOULDBLOCK) ||
		    (errno == EINTR)) {
			recverr++;
			return;
		}
		perror("recv");
		fatal = 1;
		return;
	}
	/* enforce a reasonable length */
	if (cc < (ssize_t) length_query + 2) {
		tooshort++;
		return;
	}
	/* must match the ID */
	if (memcmp(ibuf + 2 + NS_OFF_ID, &x->id, 2) != 0) {
		badid++;
		return;
	}
	/* must be a response */
	memcpy(&v, ibuf + 2 + NS_OFF_FLAGS, 2);
	v = ntohs(v);
	if ((v & NS_FLAG_QR) == 0) {
		notresp++;
		return;
	}
	if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
		perror("clock_gettime(receive)");
		fatal = 1;
		return;
	}
	/* got it: update stats */
	xrcount++;
	x->ts3 = now;
	delta = x->ts3.tv_sec - x->ts2.tv_sec;
	delta += (x->ts3.tv_nsec - x->ts2.tv_nsec) / 1e9;
	if (delta < dmin)
		dmin = delta;
	if (delta > dmax)
		dmax = delta;
	dsum += delta;
	dsumsq += delta * delta;
	v &= NS_RCODE_MASK;
	if (v >= NS_RCODE_LAST)
		v = NS_RCODE_LAST;
	rcodes[v] += 1;
}

/*
 * flush/timeout receive
 */

void
flushrecv(void)
{
	struct exchange *x;
	struct timespec now;
	int idx = xsent;
	int cnt = 5;
	double waited;

	if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
		perror("clock_gettime(receive)");
		fatal = 1;
		return;
	}

	while (--cnt >= 0) {
		if (idx < 0)
			return;
		x = xlist + idx;
		idx = x->next;
		if (x->state != X_SENT)
			abort();
		/* check for a timed-out exchange */
		waited = now.tv_sec - x->ts2.tv_sec;
		waited += (now.tv_nsec - x->ts2.tv_nsec) / 1e9;
		if (waited < losttime)
			return;
		/* garbage collect timed-out exchange */
		ISC_REMOVE(xsentl, x);
		(void) close(x->sock);
		x->sock = -1;
		collsent++;
		x->state = X_FREE;
		ISC_INSERT(xfree, xfreel, x);
	}
}

/*
 * poll receive
 */

void
pollrecv(int topoll)
{
	struct exchange *x;
	int evn, idx;

	for (evn = 0; evn < topoll; evn++) {
		idx = events[evn].data.fd;
		x = xlist + idx;
		if (x->state != X_SENT)
			continue;
		if (events[evn].events == 0)
			continue;
		ISC_REMOVE(xsentl, x);
		receiveresp(x);
		events[evn].events = 0;
		(void) close(x->sock);
		x->sock = -1;
		x->state = X_FREE;
		ISC_INSERT(xfree, xfreel, x);
	}
}

/*
 * get the TCP DNS socket descriptor (IPv4)
 */

int
getsock4(void)
{
	int sock;
	int flags;

	errno = 0;
	sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0)
		return -errno;

	/* make the socket descriptor not blocking */
	flags = fcntl(sock, F_GETFL, 0);
	if (flags == -1) {
		(void) close(sock);
		return -errno;
	}
	if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
		(void) close(sock);
		return -errno;
	}

	/* bind if wanted */
	if (locbind) {
		if (bind(sock,
			 (struct sockaddr *) &localaddr,
			 sizeof(struct sockaddr_in)) < 0) {
			(void) close(sock);
			return -errno;
		}
	}

	/* connect */
	if (connect(sock,
		    (struct sockaddr *) &serveraddr,
		    sizeof(struct sockaddr_in)) < 0) {
		if (errno != EINPROGRESS) {
			(void) close(sock);
			return -errno;
		}
	}
	return sock;
}

/*
 * connect the TCP DNS QUERY (IPv4)
 */

int
connect4(void)
{
	struct exchange *x;
	ssize_t ret;
	int idx;
	struct epoll_event ev;

	ret = clock_gettime(CLOCK_REALTIME, &last);
	if (ret < 0) {
		perror("clock_gettime(connect)");
		fatal = 1;
		return -errno;
	}

	if (xfree >= 0) {
		idx = xfree;
		x = xlist + idx;
		ISC_REMOVE(xfreel, x);
	} else if (xused < xlast) {
		idx = xused;
		x = xlist + idx;
		xused++;
	} else
		return -ENOMEM;

	if ((x->state != X_FREE) || (x->sock != -1))
		abort();

	memset(x, 0, sizeof(*x));
	memset(&ev, 0, sizeof(ev));
	x->next = -1;
	x->prev = NULL;
	x->ts0 = last;
	x->sock = getsock4();
	if (x->sock < 0) {
		ISC_INSERT(xfree, xfreel, x);
		return x->sock;
	}
	x->state = X_CONN;
	ISC_INSERT(xconn, xconnl, x);
	ev.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
	ev.data.fd = idx;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, x->sock, &ev) < 0) {
		perror("epoll_ctl");
		fatal = 1;
		return -errno;
	}
	x->order = xccount++;
	x->id = (uint16_t) random();
#if 0
	if (random_query > 0)
		x->rnd = (uint32_t) random();
#endif
	return idx;
}

/*
 * get the TCP DNS socket descriptor (IPv6)
 */

int
getsock6(void)
{
	int sock;
	int flags;

	errno = 0;
	sock = socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0)
		return -errno;

	/* make the socket descriptor not blocking */
	flags = fcntl(sock, F_GETFL, 0);
	if (flags == -1) {
		(void) close(sock);
		return -errno;
	}
	if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
		(void) close(sock);
		return -errno;
	}

	/* bind if wanted */
	if (locbind) {
		if (bind(sock,
			 (struct sockaddr *) &localaddr,
			 sizeof(struct sockaddr_in6)) < 0) {
			(void) close(sock);
			return -errno;
		}
	}

	/* connect */
	if (connect(sock,
		    (struct sockaddr *) &serveraddr,
		    sizeof(struct sockaddr_in6)) < 0) {
		if (errno != EINPROGRESS) {
			(void) close(sock);
			return -errno;
		}
	}
	return sock;
}

/*
 * connect the TCP DNS QUERY (IPv6)
 */

int
connect6(void)
{
	struct exchange *x;
	ssize_t ret;
	int idx;
	struct epoll_event ev;

	ret = clock_gettime(CLOCK_REALTIME, &last);
	if (ret < 0) {
		perror("clock_gettime(connect)");
		fatal = 1;
		return -errno;
	}

	if (xfree >= 0) {
		idx = xfree;
		x = xlist + idx;
		ISC_REMOVE(xfreel, x);
	} else if (xused < xlast) {
		idx = xused;
		x = xlist + idx;
		xused++;
	} else
		return -ENOMEM;

	memset(x, 0, sizeof(*x));
	memset(&ev, 0, sizeof(ev));
	x->next = -1;
	x->prev = NULL;
	x->ts0 = last;
	x->sock = getsock6();
	if (x->sock < 0) {
		ISC_INSERT(xfree, xfreel, x);
		return x->sock;
	}
	x->state = X_CONN;
	ISC_INSERT(xconn, xconnl, x);
	ev.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
	ev.data.fd = idx;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, x->sock, &ev) < 0) {
		perror("epoll_ctl");
		fatal = 1;
		return -errno;
	}
	x->order = xccount++;
	x->id = (uint16_t) random();
#if 0
	if (random_query > 0)
		x->rnd = (uint32_t) random();
#endif
	return idx;
}

/*
 * get the server socket address from the command line:
 *  - flags: inherited from main, 0 or AI_NUMERICHOST (for literals)
 */

void
getserveraddr(const int flags)
{
	struct addrinfo hints, *res;
	char *service;
	int ret;

	memset(&hints, 0, sizeof(hints));
	if (ipversion == 4) {
		hints.ai_family = AF_INET;
		service = "53";
	} else {
		hints.ai_family = AF_INET6;
		service = "53";
	}
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV | flags;
	hints.ai_protocol = IPPROTO_TCP;
	
	ret = getaddrinfo(servername, service, &hints, &res);
	if (ret != 0) {
		fprintf(stderr, "bad server=%s: %s\n",
			servername, gai_strerror(ret));
		exit(2);
	}
	if (res->ai_next != NULL) {
		fprintf(stderr, "ambiguous server=%s\n", servername);
		exit(2);
	}
	memcpy(&serveraddr, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
}

/*
 * get the local socket address from the command line
 */

void
getlocaladdr(void)
{
	struct addrinfo hints, *res;
	char *service;
	int ret;

	memset(&hints, 0, sizeof(hints));
	if (ipversion == 4) {
		hints.ai_family = AF_INET;
		service = "53";
	} else {
		hints.ai_family = AF_INET6;
		service = "53";
	}
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
	hints.ai_protocol = IPPROTO_TCP;
	
	ret = getaddrinfo(localname, service, &hints, &res);
	if (ret != 0) {
		fprintf(stderr,
			"bad -l<local-addr=%s>: %s\n",
			localname,
			gai_strerror(ret));
		exit(2);
	}
	/* refuse multiple addresses */
	if (res->ai_next != NULL) {
		fprintf(stderr,
			"ambiguous -l<local-addr=%s>\n",
			localname);
		exit(2);
	}
	memcpy(&localaddr, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	return;
}

/*
 * intermediate reporting
 * (note: an in-transit packet can be reported as dropped)
 */

void
reporting(void)
{
	dreport.tv_sec += report;

	if (xccount != 0) {
		printf("connect: %llu, sent: %llu, received: %llu "
		       "(embryonics: %lld, drops: %lld)",
		       (unsigned long long) xccount,
		       (unsigned long long) xscount,
		       (unsigned long long) xrcount,
		       (long long) (xccount - xscount),
		       (long long) (xscount - xrcount));
		if (xrcount != 0) {
			double avg;

			avg = dsum / xrcount;
			printf(" average: %.3f ms", avg * 1e3);
		}
	}
	printf("\n");
}

/*
 * SIGCHLD handler
 */

void
reapchild(int sig)
{
	int status;

	sig = sig;
	while (wait3(&status, WNOHANG, NULL) > 0)
		/* continue */;
}

/*
 * SIGINT handler
 */

void
interrupt(int sig)
{
	sig = sig;
	interrupted = 1;
}

/*
 * '-v' handler
 */

void
version(void)
{
	fprintf(stderr, "version 0.01\n");
}

/*
 * usage (from the wiki)
 */

void
usage(void)
{
	fprintf(stderr, "%s",
"perftcpdns [-hvX0] [-4|-6] [-r<rate>] [-t<report>] [-n<num-request>]\n"
"    [-p<test-period>] [-d<drop-time>] [-D<max-drop>] [-l<local-addr>]\n"
"    [-P<preload>] [-a<aggressivity>] [-s<seed>] [-M<memory>]\n"
"    [-T<template-file>] [-O<random-offset] [-x<diagnostic-selector>]\n"
"    [server]\n"
"\f\n"
"The [server] argument is the name/address of the DNS server to contact.\n"
"\n"
"Options:\n"
"-0: Add EDNS0 option with DO flag.\n"
"-4: TCP/IPv4 operation (default). This is incompatible with the -6 option.\n"
"-6: TCP/IPv6 operation. This is incompatible with the -4 option.\n"
"-a<aggressivity>: When the target sending rate is not yet reached,\n"
"    control how many connections are initiated before the next pause.\n"
"-d<drop-time>: Specify the time after which a query is treated as\n"
"    having been lost.  The value is given in seconds and may contain a\n"
"    fractional component.  The default is 1 second.\n"
"-h: Print this help.\n"
"-l<local-addr>: Specify the local hostname/address to use when\n"
"     communicating with the server.\n"
"-M<memory>: Size of the tables (default 60000)\n"
"-O<random-offset>: Offset of the last octet to randomize in the template.\n"
"-P<preload>: Initiate first <preload> exchanges back to back at startup.\n"
"-r<rate>: Initiate <rate> TCP DNS connections per second.  A periodic\n"
"    report is generated showing the number of exchanges which were not\n"
"    completed, as well as the average response latency.  The program\n"
"    continues until interrupted, at which point a final report is\n"
"    generated.\n"
"-s<seed>: Specify the seed for randomization, making it repeatable.\n"
"-T<template-file>: The name of a file containing the template to use\n"
"    as a stream of hexadecimal digits.\n"
"-v: Report the version number of this program.\n"
"-X: change default template to get NXDOMAIN responses.\n"
"-x<diagnostic-selector>: Include extended diagnostics in the output.\n"
"    <diagnostic-selector> is a string of single-keywords specifying\n"
"    the operations for which verbose output is desired.  The selector\n"
"    keyletters are:\n"
"   * 'a': print the decoded command line arguments\n"
"   * 'e': print the exit reason\n"
"   * 'i': print rate processing details\n"
"   * 'T': when finished, print templates\n"
"\n"
"The remaining options are used only in conjunction with -r:\n"
"\n"
"-D<max-drop>: Abort the test if more than <max-drop> requests have\n"
"    been dropped.  Use -D0 to abort if even a single request has been\n"
"    dropped.  If <max-drop> includes the suffix '%', it specifies a\n"
"    maximum percentage of requests that may be dropped before abort.\n"
"    In this case, testing of the threshold begins after 10 requests\n"
"    have been expected to be received.\n"
"-n<num-request>: Initiate <num-request> transactions.  No report is\n"
"    generated until all transactions have been initiated/waited-for,\n"
"    after which a report is generated and the program terminates.\n"
"-p<test-period>: Send requests for the given test period, which is\n"
"    specified in the same manner as -d.  This can be used as an\n"
"    alternative to -n, or both options can be given, in which case the\n"
"    testing is completed when either limit is reached.\n"
"-t<report>: Delay in seconds between two periodic reports.\n"
"\n"
"Errors:\n"
"- locallimit: reached to local system limits when sending a message.\n"
"- badconn: connection failed (from getsockopt(SO_ERROR))\n"
"- collconn: connect() timed out\n"
"- badsent: send() failed\n"
"- callsent: timed out waiting from a response\n"
"- recverr: recv() system call failed\n"
"- tooshort: received a too short message\n"
"- badid: the id mismatches between the query and the response\n"
"- notresp: doesn't receive a response\n"
"Rate stats:\n"
"- loops: number of main loop iterations\n"
"- compconn: computed number of connect() calls\n"
"- lateconn: connect() already dued when computing delay to the next one\n"
"- shortwait: no connect() to perform at the end of current iteration\n"
"\n"
"Exit status:\n"
"The exit status is:\n"
"0 on complete success.\n"
"1 for a general error.\n"
"2 if an error is found in the command line arguments.\n"
"3 if there are no general failures in operation, but one or more\n"
"  exchanges are not successfully completed.\n");
}

/*
 * main function / entry point
 */

int
main(const int argc, char * const argv[])
{
	int opt, flags = 0, ret, i;
	long long r;
	char *pc;
	extern char *optarg;
	extern int optind;

#define OPTIONS	"hv460XM:r:t:R:b:n:p:d:D:l:P:a:s:T:O:x:"

	/* decode options */
	while ((opt = getopt(argc, argv, OPTIONS)) != -1)
	switch (opt) {
	case 'h':
		usage();
		exit(0);

	case 'v':
		version();
		exit(0);

	case '0':
		edns0 = 1;
		break;

	case '4':
		if (ipversion == 6) {
			fprintf(stderr, "IP version already set to 6\n");
			usage();
			exit(2);
		}
		ipversion = 4;
		break;

	case '6':
		if (ipversion == 4) {
			fprintf(stderr, "IP version already set to 4\n");
			usage();
			exit(2);
		}
		ipversion = 6;
		break;

	case 'X':
		ixann = 1;
		break;

	case 'M':
		xlast = atoi(optarg);
		if (xlast <= 1000) {
			fprintf(stderr, "memory must be greater than 1000\n");
			usage();
			exit(2);
		}
		break;

	case 'r':
		rate = atoi(optarg);
		if (rate <= 0) {
			fprintf(stderr, "rate must be a positive integer\n");
			usage();
			exit(2);
		}
		break;

	case 't':
		report = atoi(optarg);
		if (report <= 0) {
			fprintf(stderr, "report must be a positive integer\n");
			usage();
			exit(2);
		}
		break;

	case 'R':
		r = atoll(optarg);
		if (r < 0) {
			fprintf(stderr,
				"range must not be a negative integer\n");
			usage();
			exit(2);
		}
		range = (uint32_t) r;
		if ((range != 0) && (range != UINT32_MAX)) {
			uint32_t s = range + 1;
			uint64_t b = UINT32_MAX + 1, m;

			m = (b / s) * s;
			if (m == b)
				maxrandom = 0;
			else
				maxrandom = (uint32_t) m;
		}
		break;

	case 'b':
		if (basecnt > 1) {
			fprintf(stderr, "too many bases\n");
			usage();
			exit(2);
		}
		base[basecnt] = optarg;
		/* decodebase(); */
		basecnt++;
		break;

	case 'n':
		numreq = atoi(optarg);
		if (numreq <= 0) {
			fprintf(stderr,
				"num-request must be a positive integer\n");
			usage();
			exit(2);
		}
		break;

	case 'p':
		period = atoi(optarg);
		if (period <= 0) {
			fprintf(stderr,
				"test-period must be a positive integer\n");
			usage();
			exit(2);
		}
		break;

	case 'd':
		losttime = atof(optarg);
		if (losttime <= 0.) {
			fprintf(stderr,
				"drop-time must be a positive number\n");
			usage();
			exit(2);
		}
		break;

	case 'D':
		pc = strchr(optarg, '%');
		if (pc != NULL) {
			*pc = '\0';
			maxpdrop = atof(optarg);
			if ((maxpdrop <= 0) || (maxpdrop >= 100)) {
				fprintf(stderr,
					"invalid drop-time percentage\n");
				usage();
				exit(2);
			}
			break;
		}
		maxdrop = atoi(optarg);
		if (maxdrop <= 0) {
			fprintf(stderr,
				"max-drop must be a positive integer\n");
			usage();
			exit(2);
		}
		break;

	case 'l':
		localname = optarg;
		break;

	case 'P':
		preload = atoi(optarg);
		if (preload < 0) {
			fprintf(stderr,
				"preload must not be a negative integer\n");
			usage();
			exit(2);
		}
		break;

	case 'a':
		aggressivity = atoi(optarg);
		if (aggressivity <= 0) {
			fprintf(stderr,
				"aggressivity must be a positive integer\n");
			usage();
			exit(2);
		}
		break;

	case 's':
		seeded = 1;
		seed = (unsigned int) atol(optarg);
		break;

	case 'T':
		if (templatefile != NULL) {
			fprintf(stderr, "template-file is already set\n");
			usage();
			exit(2);
		}
		templatefile = optarg;
		break;

	case 'O':
		rndoffset = atoi(optarg);
		if (rndoffset < 14) {
			fprintf(stderr,
				"random-offset must be greater than 14\n");
			usage();
			exit(2);
		}
		break;

	case 'x':
		diags = optarg;
		break;

	default:
		usage();
		exit(2);
	}

	/* adjust some global variables */
	if (ipversion == 0)
		ipversion = 4;
	if (rate == 0)
		rate = 100;
	if (xlast == 0)
		xlast = 60000;

	/* when required, print the internal view of the command line */
	if ((diags != NULL) && (strchr(diags, 'a') != NULL)) {
		printf("IPv%d", ipversion);
		printf(" rate=%d", rate);
		if (edns0 != 0)
			printf(" EDNS0");
		if (report != 0)
			printf(" report=%d", report);
		if (range != 0) {
			if (strchr(diags, 'r') != NULL)
				printf(" range=0..%d [0x%x]",
				       range,
				       (unsigned int) maxrandom);
			else
				printf(" range=0..%d", range);
		}
		if (basecnt != 0)
			for (i = 0; i < basecnt; i++)
				printf(" base[%d]='%s'", i, base[i]);
		if (numreq != 0)
			printf(" num-request=%d", numreq);
		if (period != 0)
			printf(" test-period=%d", period);
		printf(" drop-time=%g", losttime);
		if (maxdrop != 0)
			printf(" max-drop=%d", maxdrop);
		if (maxpdrop != 0.)
			printf(" max-drop=%2.2f%%", maxpdrop);
		if (preload != 0)
			printf(" preload=%d", preload);
		printf(" aggressivity=%d", aggressivity);
		if (seeded)
			printf(" seed=%u", seed);
		if (templatefile != NULL)
			printf(" template-file='%s'", templatefile);
		else if (ixann != 0)
			printf(" Xflag");
		if (rndoffset >= 0)
			printf(" rnd-offset=%d", rndoffset);
		printf(" diagnotic-selectors='%s'", diags);
		printf("\n");
	}

	/* check template file options */
	if ((templatefile == NULL) && (rndoffset >= 0)) {
		fprintf(stderr,
			"-T<template-file> must be set to "
			"use -O<random-offset>\n");
		usage();
		exit(2);
	}

	/* check various template file(s) and other condition(s) options */
	if ((templatefile != NULL) && (range > 0) && (rndoffset < 0)) {
		fprintf(stderr,
			"-O<random-offset> must be set when "
			"-T<template-file> and -R<range> are used\n");
		usage();
		exit(2);
	}

	/* get the server argument */
	if (optind < argc - 1) {
		fprintf(stderr, "extra arguments?\n");
		usage();
		exit(2);
	}
	if (optind == argc - 1)
		servername = argv[optind];

	/* handle the local '-l' address/interface */
	if (localname != NULL) {
		/* given */
		getlocaladdr();
		if ((diags != NULL) && (strchr(diags, 'a') != NULL))
			printf("local-addr='%s'\n", localname);
	}

	/* get the server socket address */
	if (servername == NULL) {
		fprintf(stderr, "server is required\n");
		usage();
		exit(2);
	}
	getserveraddr(flags);

	/* finish local/server socket address stuff and print it */
	if ((diags != NULL) && (strchr(diags, 'a') != NULL))
		printf("server='%s'\n", servername);
	if ((localname != NULL) &&
	    (diags != NULL) && (strchr(diags, 'a') != NULL)) {
		char addr[NI_MAXHOST];

		ret = getnameinfo((struct sockaddr *) &localaddr,
				  sizeof(localaddr),
				  addr,
				  NI_MAXHOST,
				  NULL,
				  0,
				  NI_NUMERICHOST);
		if (ret != 0) {
			fprintf(stderr,
				"can't get the local address: %s\n",
				gai_strerror(ret));
			exit(1);
		}
		printf("local address='%s'\n", addr);
	}

	/* initialize exchange structures */
	inits();

	/* get the socket descriptor and template(s) */
	if (templatefile == NULL)
		build_template_query();
	else
		get_template_query();

	/* boot is done! */
	if (clock_gettime(CLOCK_REALTIME, &boot) < 0) {
		perror("clock_gettime(boot)");
		exit(1);
	}

	/* compute the next intermediate reporting date */
	if (report != 0) {
		dreport.tv_sec = boot.tv_sec + report;
		dreport.tv_nsec = boot.tv_nsec;
	}

	/* seed the random generator */
	if (seeded == 0)
		seed = (unsigned int) (boot.tv_sec + boot.tv_nsec);
	srandom(seed);

	/* preload the server with at least one connection */
	compconn = preload + 1;
	for (i = 0; i <= preload; i++) {
		if (ipversion == 4)
			ret = connect4();
		else
			ret = connect6();
		if (ret < 0) {
			/* failure at the first connection is fatal */
			if (i == 0) {
				fprintf(stderr,
					"initial connect failed: %s\n",
					strerror(-ret));
				exit(1);
			}
			if ((ret == -EAGAIN) ||
			    (ret == -EWOULDBLOCK) ||
			    (ret == -ENOBUFS) ||
			    (ret == -ENOMEM))
				locallimit++;
			fprintf(stderr,
				"preload connect failed: %s\n",
				strerror(-ret));
			break;
		}
	}

	/* required only before the interrupted flag check */
	(void) signal(SIGINT, interrupt);

	/* main loop */
	for (;;) {
		struct timespec now, ts;
		fd_set rfds;
		int nfds;

		/* immediate loop exit conditions */
		if (interrupted) {
			if ((diags != NULL) && (strchr(diags, 'e') != NULL))
				printf("interrupted\n");
			break;
		}
		if (fatal) {
			if ((diags != NULL) && (strchr(diags, 'e') != NULL))
				printf("got a fatal error\n");
			break;
		}

		loops++;

		/* get the date and use it */
		if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
			perror("clock_gettime(now)");
			fatal = 1;
			continue;
		}
		if ((period != 0) &&
		    ((boot.tv_sec + period < now.tv_sec) ||
		     ((boot.tv_sec + period == now.tv_sec) &&
		      (boot.tv_nsec < now.tv_nsec)))) {
			if ((diags != NULL) && (strchr(diags, 'e') != NULL))
				printf("reached test-period\n");
			break;
		}
		if ((report != 0) &&
		    ((dreport.tv_sec < now.tv_sec) ||
		     ((dreport.tv_sec == now.tv_sec) &&
		      (dreport.tv_nsec < now.tv_nsec))))
			reporting();

		/* compute the delay for the next connection */
		due = last;
		if (rate == 1)
			due.tv_sec += 1;
		else
			due.tv_nsec += 1010000000 / rate;
		while (due.tv_nsec >= 1000000000) {
			due.tv_sec += 1;
			due.tv_nsec -= 1000000000;
		}
		ts = due;
		ts.tv_sec -= now.tv_sec;
		ts.tv_nsec -= now.tv_nsec;
		while (ts.tv_nsec < 0) {
			ts.tv_sec -= 1;
			ts.tv_nsec += 1000000000;
		}
		/* the connection was already due? */
		if (ts.tv_sec < 0) {
			ts.tv_sec = ts.tv_nsec = 0;
			lateconn++;
		}

		/* pselect() */
		FD_ZERO(&rfds);
		FD_SET(epoll_fd, &rfds);
		ret = pselect(epoll_fd + 1, &rfds, NULL, NULL, &ts, NULL);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("pselect");
			fatal = 1;
			continue;
		}
			
		/* epoll_wait() */
		memset(events, 0, sizeof(events));
		nfds = epoll_wait(epoll_fd, events, EVENTS_CNT, 0);
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll");
			fatal = 1;
			continue;
		}

		/* connection(s) to finish */
		pollconnect(nfds);
		if (fatal)
			continue;
		flushconnect();
		if (fatal)
			continue;

		/* packet(s) to receive */
		pollrecv(nfds);
		if (fatal)
			continue;
		flushrecv();
		if (fatal)
			continue;

		/* packet(s) to send */
		pollsend();
		if (fatal)
			continue;

		/* check receive loop exit conditions */
		if ((numreq != 0) && ((int) xscount >= numreq)) {
			if ((diags != NULL) && (strchr(diags, 'e') != NULL))
				printf("reached num-request\n");
			break;
		}
		if ((maxdrop != 0) &&
		    ((int) (xscount - xrcount) > maxdrop)) {
			if ((diags != NULL) && (strchr(diags, 'e') != NULL))
				printf("reached max-drop (absolute)\n");
			break;
		}
		if ((maxpdrop != 0.) &&
		    (xscount > 10) &&
		    (((100. * (xscount - xrcount)) / xscount) > maxpdrop)) {
			if ((diags != NULL) && (strchr(diags, 'e') != NULL))
				printf("reached max-drop (percent)\n");
			break;
		}

		/* compute how many connections to open */
		if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
			perror("clock_gettime(now2)");
			fatal = 1;
			continue;
		}
		if ((now.tv_sec > due.tv_sec) ||
		    ((now.tv_sec == due.tv_sec) &&
		     (now.tv_nsec >= due.tv_nsec))) {
			double toconnect;

			toconnect = (now.tv_nsec - due.tv_nsec) / 1e9;
			toconnect += now.tv_sec - due.tv_sec;
			toconnect *= rate;
			toconnect++;
			if (toconnect > (double) aggressivity)
				i = aggressivity;
			else
				i = (int) toconnect;
			compconn += i;
			/* open connections */
			while (i-- > 0) {
				if (ipversion == 4)
					ret = connect4();
				else
					ret = connect6();
				if (ret < 0) {
					if ((ret == -EAGAIN) ||
					    (ret == -EWOULDBLOCK) ||
					    (ret == -ENOBUFS) ||
					    (ret == -ENOMEM))
						locallimit++;
					fprintf(stderr,
						"connect: %s\n",
						strerror(-ret));
					break;
				}
			}
		} else
			/* there was no connection to open */
			shortwait++;
	}

	/* after main loop: finished */
	if (clock_gettime(CLOCK_REALTIME, &finished) < 0)
		perror("clock_gettime(finished)");

	/* main statictics */
	printf("connect: %llu, sent: %llu, received: %llu "
	       "(embryonics: %lld, drops: %lld)\n",
	       (unsigned long long) xccount,
	       (unsigned long long) xscount,
	       (unsigned long long) xrcount,
	       (long long) (xccount - xscount),
	       (long long) (xscount - xrcount));
	printf("local limits: %llu, bad connects: %llu, "
	       "connect time outs: %llu\n",
	       (unsigned long long) locallimit,
	       (unsigned long long) badconn,
	       (unsigned long long) collconn);
	printf("bad sends: %llu, bad recvs: %llu, recv time outs: %llu\n",
	       (unsigned long long) badsent,
	       (unsigned long long) recverr,
	       (unsigned long long) collsent);
	printf("too shorts: %llu, bad IDs: %llu, not responses: %llu\n",
	       (unsigned long long) tooshort,
	       (unsigned long long) badid,
	       (unsigned long long) notresp);
	printf("rcode counters:\n noerror: %llu, formerr: %llu, "
	       "servfail: %llu\n "
	       "nxdomain: %llu, noimp: %llu, refused: %llu, others: %llu\n",
	       (unsigned long long) rcodes[NS_RCODE_NOERROR],
	       (unsigned long long) rcodes[NS_RCODE_FORMERR],
	       (unsigned long long) rcodes[NS_RCODE_SERVFAIL],
	       (unsigned long long) rcodes[NS_RCODE_NXDOMAIN],
	       (unsigned long long) rcodes[NS_RCODE_NOIMP],
	       (unsigned long long) rcodes[NS_RCODE_REFUSED],
	       (unsigned long long) rcodes[NS_RCODE_LAST]);

	/* print the rate */
	if (finished.tv_sec != 0) {
		double dall, erate;

		dall = (finished.tv_nsec - boot.tv_nsec) / 1e9;
		dall += finished.tv_sec - boot.tv_sec;
		erate = xrcount / dall;
		printf("rate: %f (expected %d)\n", erate, rate);
	}

	/* rate processing instrumentation */
	if ((diags != NULL) && (strchr(diags, 'i') != NULL)) {
		printf("loops: %llu, compconn: %llu, "
		       "lateconn: %llu, shortwait: %llu\n",
		       (unsigned long long) loops,
		       (unsigned long long) compconn,
		       (unsigned long long) lateconn,
		       (unsigned long long) shortwait);
		printf("badconn: %llu, collconn: %llu, "
		       "recverr: %llu, collsent: %llu\n",
		       (unsigned long long) badconn,
		       (unsigned long long) collconn,
		       (unsigned long long) recverr,
		       (unsigned long long) collsent);
		printf("memory: used(%d) / allocated(%d)\n",
		       xused, xlast);
	}

	/* round-time trip statistics */
	if (xrcount != 0) {
		double avg, stddev;
		
		avg = dsum / xrcount;
		stddev = sqrt(dsumsq / xrcount - avg * avg);
		printf("RTT: min/avg/max/stddev:  %.3f/%.3f/%.3f/%.3f ms\n",
		       dmin * 1e3, avg * 1e3, dmax * 1e3, stddev * 1e3);
	}
	printf("\n");

	/* template(s) */
	if ((diags != NULL) && (strchr(diags, 'T') != NULL)) {
		size_t n;

		printf("length = 0x%zx\n", length_query);
		if (random_query > 0)
			printf("random offset = %zu\n", random_query);
		printf("content:\n");
		for (n = 0; n < length_query; n++) {
			printf("%s%02hhx",
			       (n & 15) == 0 ? "" : " ",
			       template_query[n]);
			if ((n & 15) == 15)
				printf("\n");
		}
		if ((n & 15) != 15)
			printf("\n");
		printf("\n");
	}

	/* compute the exit code (and exit) */
	if (fatal)
		exit(1);
	else if (xscount == xrcount)
		exit(0);
	else
		exit(3);
}