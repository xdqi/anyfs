// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lkl_loopbench.c — both ends of the TCP path live inside LKL.
 * Splits the cost of the proxy vs. the LKL kernel TCP stack:
 *
 *   if this is also ~900 MB/s, LKL is the wall.
 *   if this is much faster, the proxy share matters.
 *
 * Server thread: in-LKL accept loop, per-conn writer blasts zeros.
 * Client thread: in-LKL connect to 127.0.0.1:port, loop lkl_sys_read,
 * count bytes. No host syscalls on the data path.
 */

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <lkl.h>
#include <lkl_host.h>

#include "anyfs.h"

#define LKL_PORT 12346
#define BUFSZ (1024 * 1024)

static int g_server_fd = -1;

static void* server_writer(void* arg)
{
	int fd = (int)(intptr_t)arg;
	char* buf = calloc(1, BUFSZ);
	for (;;) {
		long n = lkl_sys_write(fd, buf, BUFSZ);
		if (n <= 0)
			break;
	}
	lkl_sys_close(fd);
	free(buf);
	return NULL;
}

static void* server_loop(void* arg)
{
	(void)arg;
	for (;;) {
		struct lkl_sockaddr_in cli;
		int alen = sizeof(cli);
		long c = lkl_sys_accept(g_server_fd, (struct lkl_sockaddr*)&cli,
					&alen);
		if (c < 0)
			break;
		pthread_t t;
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		pthread_create(&t, &a, server_writer, (void*)(intptr_t)c);
		pthread_attr_destroy(&a);
	}
	return NULL;
}

static int start_server(void)
{
	long s = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM, 0);
	int one = 1;
	lkl_sys_setsockopt((int)s, LKL_SOL_SOCKET, LKL_SO_REUSEADDR,
			   (char*)&one, sizeof(one));
	struct lkl_sockaddr_in addr = {
	    .sin_family = LKL_AF_INET,
	    .sin_port = __lkl__cpu_to_be16(LKL_PORT),
	    .sin_addr.lkl_s_addr = 0,
	};
	lkl_sys_bind((int)s, (struct lkl_sockaddr*)&addr, sizeof(addr));
	lkl_sys_listen((int)s, 16);
	g_server_fd = (int)s;
	pthread_t t;
	pthread_create(&t, NULL, server_loop, NULL);
	pthread_detach(t);
	return 0;
}

struct client_result {
	uint64_t bytes;
	double seconds;
};

static void* client_reader(void* arg)
{
	struct client_result* r = arg;
	long target = r->bytes; /* caller sets target in bytes */

	long s = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM, 0);
	struct lkl_sockaddr_in addr = {
	    .sin_family = LKL_AF_INET,
	    .sin_port = __lkl__cpu_to_be16(LKL_PORT),
	    .sin_addr.lkl_s_addr = __lkl__cpu_to_be32(0x7f000001),
	};
	long rc =
	    lkl_sys_connect((int)s, (struct lkl_sockaddr*)&addr, sizeof(addr));
	if (rc < 0) {
		fprintf(stderr, "client connect: %s\n", lkl_strerror(rc));
		r->bytes = 0;
		r->seconds = 0;
		return NULL;
	}

	char* buf = malloc(BUFSZ);
	uint64_t got = 0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while ((long)got < target) {
		long n = lkl_sys_read((int)s, buf, BUFSZ);
		if (n <= 0)
			break;
		got += (uint64_t)n;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	lkl_sys_close((int)s);
	free(buf);

	r->bytes = got;
	r->seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	return NULL;
}

int main(int argc, char** argv)
{
	int parallel = argc > 1 ? atoi(argv[1]) : 1;
	long mb = argc > 2 ? atol(argv[2]) : 2000;

	signal(SIGPIPE, SIG_IGN);

	AnyfsKernelOpts k = {.mem_mb = 0 /* anyfs default (32M) */,
			     .loglevel = 4};
	if (anyfs_kernel_init(&k) != 0)
		return 1;
	lkl_if_up(1);
	start_server();

	/* Make sure server is listening before clients connect. */
	struct timespec ts = {.tv_nsec = 100 * 1000 * 1000};
	nanosleep(&ts, NULL);

	pthread_t* clients = calloc(parallel, sizeof(*clients));
	struct client_result* res = calloc(parallel, sizeof(*res));
	long per = (mb * 1024L * 1024L) / parallel;
	for (int i = 0; i < parallel; i++) {
		res[i].bytes = per;
		pthread_create(&clients[i], NULL, client_reader, &res[i]);
	}

	struct timespec w0, w1;
	clock_gettime(CLOCK_MONOTONIC, &w0);
	uint64_t total = 0;
	for (int i = 0; i < parallel; i++) {
		pthread_join(clients[i], NULL);
		total += res[i].bytes;
	}
	clock_gettime(CLOCK_MONOTONIC, &w1);
	double wall = (w1.tv_sec - w0.tv_sec) + (w1.tv_nsec - w0.tv_nsec) / 1e9;

	for (int i = 0; i < parallel; i++) {
		double mbps =
		    (res[i].bytes / (1024.0 * 1024.0)) / res[i].seconds;
		printf("  client %d: %llu MB in %.3f s -> %.0f MB/s\n", i,
		       (unsigned long long)(res[i].bytes / (1024 * 1024)),
		       res[i].seconds, mbps);
	}
	printf("aggregate: %llu MB in %.3f s -> %.0f MB/s\n",
	       (unsigned long long)(total / (1024 * 1024)), wall,
	       (total / (1024.0 * 1024.0)) / wall);

	anyfs_kernel_halt();
	return 0;
}
