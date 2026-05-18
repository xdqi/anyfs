// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * host_proxy.c — host TCP listener splicing to an LKL TCP server.
 *
 * Two long-lived threads only:
 *   host  thread: all host syscalls (epoll, accept, read, send, close)
 *   LKL   thread: all lkl_sys_* calls (poll, socket, connect, read, write)
 * Per-conn 4 MiB SPSC ring each direction. Single producer / consumer so
 * release/acquire on head/tail suffices — no per-ring mutex, no
 * host_ops futex contention (exactly one caller from each side).
 *
 * Forced-close path: when a peer dies, the surviving side sets
 * lkl_close_req; the LKL thread does a real lkl_sys_close, not a
 * half-shutdown. A SHUT_WR does NOT make a pure-writer kthread peer
 * (ksmbd, zerobench) exit — it never reads, so it never sees FIN.
 * It only stops on -EPIPE from its next send.
 */

#define _GNU_SOURCE
#include "host_proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>

#define RING_CAP (4u * 1024u * 1024u)
#define RING_MASK (RING_CAP - 1)
#define MAX_CONNS 256

struct ring {
	char* buf;
	_Atomic uint64_t head, tail;
};

struct conn {
	int host_fd, lkl_fd;
	struct ring h2l, l2h;
	_Atomic int host_read_eof, lkl_read_eof;
	_Atomic int host_wr_shut, lkl_wr_shut;
	_Atomic int lkl_close_req;
	int slot;
};

static struct {
	int listen_fd, epoll_fd;
	int host_wake_r, host_wake_w;
	int lkl_wake_r, lkl_wake_w;
	uint16_t lkl_port;
	pthread_t host_tid, lkl_tid;
	_Atomic int running;
	pthread_mutex_t lock;
	struct conn* conns[MAX_CONNS];
} G;

static char g_listen_tag, g_wake_tag;
#define TAG_LISTEN ((void*)&g_listen_tag)
#define TAG_WAKE ((void*)&g_wake_tag)

static void wake_host(void)
{
	char b = 1;
	(void)write(G.host_wake_w, &b, 1);
}
static void wake_lkl(void)
{
	char b = 1;
	(void)lkl_sys_write(G.lkl_wake_w, &b, 1);
}

/* ── SPSC ring helpers ──────────────────────────────────────────────── */

static size_t ring_writable(struct ring* r, char** dst)
{
	uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	size_t freeb = RING_CAP - (size_t)(h - t);
	size_t off = (size_t)(h & RING_MASK);
	size_t until = RING_CAP - off;
	*dst = r->buf + off;
	return freeb < until ? freeb : until;
}
static size_t ring_readable(struct ring* r, char** src)
{
	uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	size_t avail = (size_t)(h - t);
	size_t off = (size_t)(t & RING_MASK);
	size_t until = RING_CAP - off;
	*src = r->buf + off;
	return avail < until ? avail : until;
}
static void ring_pcommit(struct ring* r, size_t n)
{
	uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	atomic_store_explicit(&r->head, h + n, memory_order_release);
}
static void ring_ccommit(struct ring* r, size_t n)
{
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	atomic_store_explicit(&r->tail, t + n, memory_order_release);
}
static int ring_empty(struct ring* r)
{
	return atomic_load_explicit(&r->head, memory_order_acquire) ==
	       atomic_load_explicit(&r->tail, memory_order_acquire);
}
static void ring_drain(struct ring* r)
{
	uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	atomic_store_explicit(&r->tail, h, memory_order_release);
}

/* ── Conn lifecycle ─────────────────────────────────────────────────── */

static int conn_attach(struct conn* c)
{
	pthread_mutex_lock(&G.lock);
	for (int i = 0; i < MAX_CONNS; i++)
		if (!G.conns[i]) {
			G.conns[i] = c;
			c->slot = i;
			pthread_mutex_unlock(&G.lock);
			return 0;
		}
	pthread_mutex_unlock(&G.lock);
	return -1;
}

static void conn_free(struct conn* c)
{
	pthread_mutex_lock(&G.lock);
	if (c->slot >= 0 && G.conns[c->slot] == c)
		G.conns[c->slot] = NULL;
	pthread_mutex_unlock(&G.lock);
	if (c->host_fd >= 0)
		close(c->host_fd);
	if (c->lkl_fd >= 0)
		lkl_sys_close(c->lkl_fd);
	free(c->h2l.buf);
	free(c->l2h.buf);
	free(c);
}

static int conn_done(struct conn* c)
{
	return atomic_load(&c->host_read_eof) &&
	       atomic_load(&c->host_wr_shut) && atomic_load(&c->lkl_read_eof) &&
	       atomic_load(&c->lkl_wr_shut) && ring_empty(&c->h2l) &&
	       ring_empty(&c->l2h);
}

/* ── LKL thread ─────────────────────────────────────────────────────── */

static void lkl_connect_new(int* notify)
{
	pthread_mutex_lock(&G.lock);
	struct conn* snap[MAX_CONNS];
	int n = 0;
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn* c = G.conns[i];
		if (c && c->lkl_fd < 0 && !atomic_load(&c->lkl_read_eof))
			snap[n++] = c;
	}
	pthread_mutex_unlock(&G.lock);

	for (int i = 0; i < n; i++) {
		struct conn* c = snap[i];
		long s = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM, 0);
		if (s >= 0) {
			long fl = lkl_sys_fcntl(s, LKL_F_GETFL, 0);
			lkl_sys_fcntl(s, LKL_F_SETFL, fl | LKL_O_NONBLOCK);
			struct lkl_sockaddr_in sin = {0};
			sin.sin_family = LKL_AF_INET;
			sin.sin_port = htons(G.lkl_port);
			sin.sin_addr.lkl_s_addr = htonl(0x7f000001);
			long r = lkl_sys_connect(s, (struct lkl_sockaddr*)&sin,
						 sizeof(sin));
			if (r < 0 && r != -LKL_EINPROGRESS) {
				lkl_sys_close(s);
				s = r;
			}
		}
		if (s < 0) {
			atomic_store(&c->host_read_eof, 1);
			atomic_store(&c->host_wr_shut, 1);
			atomic_store(&c->lkl_read_eof, 1);
			atomic_store(&c->lkl_wr_shut, 1);
		} else {
			c->lkl_fd = (int)s;
		}
		*notify = 1;
	}
}

static void lkl_do_force_close(int* notify)
{
	int fds[MAX_CONNS];
	int n = 0;
	pthread_mutex_lock(&G.lock);
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn* c = G.conns[i];
		if (!c || c->lkl_fd < 0 || !atomic_load(&c->lkl_close_req))
			continue;
		fds[n++] = c->lkl_fd;
		c->lkl_fd = -1;
		atomic_store(&c->lkl_read_eof, 1);
		atomic_store(&c->lkl_wr_shut, 1);
		/* Both rings undeliverable: lkl_fd is gone (h2l) and
		 * host_wr_shut means l2h has nowhere to go. Drop them so
		 * conn_done can fire. */
		ring_drain(&c->h2l);
		ring_drain(&c->l2h);
	}
	pthread_mutex_unlock(&G.lock);
	for (int i = 0; i < n; i++)
		lkl_sys_close(fds[i]);
	if (n)
		*notify = 1;
}

static void lkl_io(struct conn* c, int* notify)
{
	char* p;
	size_t k;
	if (!atomic_load(&c->lkl_read_eof)) {
		while ((k = ring_writable(&c->l2h, &p)) != 0) {
			long got = lkl_sys_read(c->lkl_fd, p, k);
			if (got > 0) {
				ring_pcommit(&c->l2h, got);
				*notify = 1;
				if ((size_t)got < k)
					break;
				continue;
			}
			if (got == -LKL_EAGAIN)
				break;
			if (got == -LKL_EINTR)
				continue;
			atomic_store(&c->lkl_read_eof, 1);
			*notify = 1;
			break;
		}
	}
	if (!atomic_load(&c->lkl_wr_shut)) {
		while ((k = ring_readable(&c->h2l, &p)) != 0) {
			long w = lkl_sys_write(c->lkl_fd, p, k);
			if (w > 0) {
				ring_ccommit(&c->h2l, w);
				*notify = 1;
				if ((size_t)w < k)
					break;
				continue;
			}
			if (w == -LKL_EAGAIN)
				break;
			if (w == -LKL_EINTR)
				continue;
			atomic_store(&c->lkl_wr_shut, 1);
			break;
		}
	}
	/* Half-shut propagation: host EOF + h2l drained → tell LKL peer.
	 * (Pure-writer kthread peers ignore this; the host side will then
	 * also signal lkl_close_req when its send fails.) */
	if (!atomic_load(&c->lkl_wr_shut) && atomic_load(&c->host_read_eof) &&
	    ring_empty(&c->h2l)) {
		lkl_sys_shutdown(c->lkl_fd, 1 /* SHUT_WR */);
		atomic_store(&c->lkl_wr_shut, 1);
		*notify = 1;
	}
}

static void* lkl_thread_fn(void* arg)
{
	(void)arg;
	static struct lkl_pollfd pfds[1 + MAX_CONNS];
	struct conn* cset[MAX_CONNS];

	while (atomic_load(&G.running)) {
		int notify = 0;
		lkl_connect_new(&notify);
		lkl_do_force_close(&notify);

		pfds[0].fd = G.lkl_wake_r;
		pfds[0].events = LKL_POLLIN;
		pfds[0].revents = 0;
		int cn = 0;
		pthread_mutex_lock(&G.lock);
		for (int i = 0; i < MAX_CONNS; i++)
			if (G.conns[i] && G.conns[i]->lkl_fd >= 0)
				cset[cn++] = G.conns[i];
		pthread_mutex_unlock(&G.lock);

		for (int i = 0; i < cn; i++) {
			struct conn* c = cset[i];
			char* p;
			short ev = 0;
			if (!atomic_load(&c->lkl_read_eof) &&
			    ring_writable(&c->l2h, &p))
				ev |= LKL_POLLIN;
			if (!atomic_load(&c->lkl_wr_shut) &&
			    ring_readable(&c->h2l, &p))
				ev |= LKL_POLLOUT;
			pfds[1 + i].fd = c->lkl_fd;
			pfds[1 + i].events = ev;
			pfds[1 + i].revents = 0;
		}

		long pr = lkl_sys_poll(pfds, 1 + cn, 1000);
		if (pr < 0) {
			if (pr == -LKL_EINTR)
				continue;
			break;
		}

		if (pfds[0].revents & LKL_POLLIN) {
			char buf[64];
			while (lkl_sys_read(G.lkl_wake_r, buf, sizeof(buf)) >
			       0) {
			}
		}
		for (int i = 0; i < cn; i++) {
			struct conn* c = cset[i];
			if (pfds[1 + i].revents &
			    (LKL_POLLERR | LKL_POLLHUP | LKL_POLLNVAL)) {
				atomic_store(&c->lkl_read_eof, 1);
				atomic_store(&c->lkl_wr_shut, 1);
				notify = 1;
			}
			lkl_io(c, &notify);
		}
		if (notify)
			wake_host();
	}
	return NULL;
}

/* ── Host thread ────────────────────────────────────────────────────── */

static void host_io(struct conn* c, int* notify)
{
	char* p;
	size_t k;
	if (!atomic_load(&c->host_read_eof)) {
		while ((k = ring_writable(&c->h2l, &p)) != 0) {
			ssize_t got = read(c->host_fd, p, k);
			if (got > 0) {
				ring_pcommit(&c->h2l, got);
				*notify = 1;
				if ((size_t)got < k)
					break;
				continue;
			}
			if (got == 0) {
				atomic_store(&c->host_read_eof, 1);
				*notify = 1;
				break;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			atomic_store(&c->host_read_eof, 1);
			*notify = 1;
			break;
		}
	}
	if (!atomic_load(&c->host_wr_shut)) {
		while ((k = ring_readable(&c->l2h, &p)) != 0) {
			ssize_t w = send(c->host_fd, p, k, MSG_NOSIGNAL);
			if (w > 0) {
				ring_ccommit(&c->l2h, w);
				*notify = 1;
				if ((size_t)w < k)
					break;
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			/* Send failed (EPIPE/RST) — peer is dead. Force-close
			 * lkl_fd so any pure-writer kthread peer sees -EPIPE on
			 * next send. */
			atomic_store(&c->host_wr_shut, 1);
			atomic_store(&c->host_read_eof, 1);
			if (!atomic_load(&c->lkl_read_eof))
				atomic_store(&c->lkl_close_req, 1);
			*notify = 1;
			break;
		}
	}
	if (!atomic_load(&c->host_wr_shut) && atomic_load(&c->lkl_read_eof) &&
	    ring_empty(&c->l2h)) {
		shutdown(c->host_fd, SHUT_WR);
		atomic_store(&c->host_wr_shut, 1);
	}
}

static void host_accept(int* notify)
{
	for (;;) {
		int cfd = accept4(G.listen_fd, NULL, NULL,
				  SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (cfd < 0)
			break;
		int one = 1;
		setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		struct conn* c = calloc(1, sizeof(*c));
		if (!c) {
			close(cfd);
			continue;
		}
		c->host_fd = cfd;
		c->lkl_fd = -1;
		c->slot = -1;
		c->h2l.buf = malloc(RING_CAP);
		c->l2h.buf = malloc(RING_CAP);
		if (!c->h2l.buf || !c->l2h.buf || conn_attach(c) < 0) {
			free(c->h2l.buf);
			free(c->l2h.buf);
			free(c);
			close(cfd);
			continue;
		}
		struct epoll_event ev = {
		    .events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP,
		    .data.ptr = c,
		};
		epoll_ctl(G.epoll_fd, EPOLL_CTL_ADD, c->host_fd, &ev);
		*notify = 1;
	}
}

static void* host_thread_fn(void* arg)
{
	(void)arg;
	struct epoll_event ev = {.events = EPOLLIN, .data.ptr = TAG_LISTEN};
	epoll_ctl(G.epoll_fd, EPOLL_CTL_ADD, G.listen_fd, &ev);
	ev.data.ptr = TAG_WAKE;
	epoll_ctl(G.epoll_fd, EPOLL_CTL_ADD, G.host_wake_r, &ev);

	while (atomic_load(&G.running)) {
		struct epoll_event evs[64];
		int n = epoll_wait(G.epoll_fd, evs, 64, 1000);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		int notify = 0;

		for (int i = 0; i < n; i++) {
			void* p = evs[i].data.ptr;
			if (p == TAG_LISTEN) {
				host_accept(&notify);
				continue;
			}
			if (p == TAG_WAKE) {
				char buf[64];
				while (read(G.host_wake_r, buf, sizeof(buf)) >
				       0) {
				}
				continue;
			}
			struct conn* c = p;
			if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
				atomic_store(&c->host_read_eof, 1);
				atomic_store(&c->host_wr_shut, 1);
				if (!atomic_load(&c->lkl_read_eof))
					atomic_store(&c->lkl_close_req, 1);
				notify = 1;
			} else if (evs[i].events & EPOLLRDHUP) {
				atomic_store(&c->host_read_eof, 1);
				notify = 1;
			}
		}

		/* Single sweep: progress every live conn (covers both epoll
		 * events and LKL-side fills that don't fire host_fd events),
		 * then free anything drained. */
		struct conn* all[MAX_CONNS];
		int nall = 0;
		pthread_mutex_lock(&G.lock);
		for (int i = 0; i < MAX_CONNS; i++)
			if (G.conns[i])
				all[nall++] = G.conns[i];
		pthread_mutex_unlock(&G.lock);

		for (int i = 0; i < nall; i++)
			if (all[i]->lkl_fd >= 0)
				host_io(all[i], &notify);
		for (int i = 0; i < nall; i++)
			if (conn_done(all[i])) {
				epoll_ctl(G.epoll_fd, EPOLL_CTL_DEL,
					  all[i]->host_fd, NULL);
				conn_free(all[i]);
			}

		if (notify)
			wake_lkl();
	}
	return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int host_proxy_start(uint16_t host_port, uint16_t lkl_port)
{
	signal(SIGPIPE, SIG_IGN);
	pthread_mutex_init(&G.lock, NULL);

	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s < 0)
		return -1;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sin = {0};
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(host_port);
	if (bind(s, (struct sockaddr*)&sin, sizeof(sin)) < 0 ||
	    listen(s, 128) < 0) {
		fprintf(stderr, "host_proxy: bind/listen :%u: %s\n", host_port,
			strerror(errno));
		close(s);
		return -1;
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	int hpipe[2], lpipe[2] = {-1, -1};
	if (ep < 0 || pipe2(hpipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		close(s);
		if (ep >= 0)
			close(ep);
		return -1;
	}
	long pr = lkl_sys_pipe2(lpipe, LKL_O_NONBLOCK);
	if (pr < 0) {
		close(s);
		close(ep);
		close(hpipe[0]);
		close(hpipe[1]);
		return -1;
	}

	G.listen_fd = s;
	G.lkl_port = lkl_port;
	G.epoll_fd = ep;
	G.host_wake_r = hpipe[0];
	G.host_wake_w = hpipe[1];
	G.lkl_wake_r = lpipe[0];
	G.lkl_wake_w = lpipe[1];
	atomic_store(&G.running, 1);

	if (pthread_create(&G.lkl_tid, NULL, lkl_thread_fn, NULL) != 0 ||
	    pthread_create(&G.host_tid, NULL, host_thread_fn, NULL) != 0) {
		atomic_store(&G.running, 0);
		wake_lkl();
		return -1;
	}
	return 0;
}

void host_proxy_stop(void)
{
	if (!atomic_load(&G.running))
		return;
	atomic_store(&G.running, 0);
	if (G.listen_fd >= 0) {
		shutdown(G.listen_fd, SHUT_RDWR);
		close(G.listen_fd);
		G.listen_fd = -1;
	}
	wake_host();
	wake_lkl();
	pthread_join(G.host_tid, NULL);
	pthread_join(G.lkl_tid, NULL);
	if (G.epoll_fd >= 0)
		close(G.epoll_fd);
	if (G.host_wake_r >= 0)
		close(G.host_wake_r);
	if (G.host_wake_w >= 0)
		close(G.host_wake_w);
	if (G.lkl_wake_r >= 0)
		lkl_sys_close(G.lkl_wake_r);
	if (G.lkl_wake_w >= 0)
		lkl_sys_close(G.lkl_wake_w);
}
