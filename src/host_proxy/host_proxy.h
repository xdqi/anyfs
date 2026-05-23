/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * host_proxy.h — host TCP listener that splices to an LKL TCP server.
 *
 * Replaces the libslirp NAT+hostfwd data path. Each accepted host
 * connection gets its own LKL socket connected to 127.0.0.1:<lkl_port>
 * inside the LKL kernel; two threads bidirectionally copy bytes between
 * the host fd and the LKL fd. libslirp is not on the data path at all.
 */
#ifndef ANYFS_HOST_PROXY_H
#define ANYFS_HOST_PROXY_H

#include <stdint.h>

/*
 * Start a listener on host *:host_port and proxy every accepted connection
 * to 127.0.0.1:lkl_port inside the running LKL kernel. Returns 0 on success.
 * Must be called after LKL is initialised and the in-kernel server is
 * actually listening (otherwise early connects get ECONNREFUSED).
 */
int host_proxy_start(uint16_t host_port, uint16_t lkl_port);

/*
 * Stop accepting new connections and tear down the listener thread.
 * In-flight connections keep running until both halves see EOF/error;
 * they are not force-killed.
 */
void host_proxy_stop(void);

#endif
