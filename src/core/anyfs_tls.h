/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * anyfs_tls.h — point the statically linked OpenSSL at the host's CA bundle.
 *
 * The linux-amd64 build links OpenSSL and curl statically with no CA bundle
 * compiled in. OpenSSL's defaults (OPENSSLDIR=/etc/ssl) cover Debian-family
 * hosts; RHEL/Fedora and SUSE keep the bundle elsewhere, and OpenSSL honours
 * SSL_CERT_FILE, so set that before the first TLS connection.
 */
#ifndef ANYFS_TLS_H
#define ANYFS_TLS_H

/* Returns the first readable path in the NULL-terminated `candidates`, or
 * NULL when SSL_CERT_FILE is already set or none exists. Exposed for tests. */
const char* anyfs_tls_ca_pick(const char* const* candidates);

/* Sets SSL_CERT_FILE to the host's bundle unless the user set it. Linux only;
 * a no-op elsewhere. Call once, before any thread may use TLS. */
void anyfs_tls_ca_init(void);

#endif
