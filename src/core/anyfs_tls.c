// SPDX-License-Identifier: GPL-2.0-or-later
/* anyfs_tls.c — see anyfs_tls.h. */
#include "anyfs_tls.h"
#include "anyfs_u8.h"

#include <stdlib.h>
#include <unistd.h>

const char* anyfs_tls_ca_pick(const char* const* candidates)
{
	if (anyfs_u8_getenv("SSL_CERT_FILE"))
		return NULL;
	for (; *candidates; candidates++)
		if (anyfs_u8_access(*candidates, R_OK) == 0)
			return *candidates;
	return NULL;
}

void anyfs_tls_ca_init(void)
{
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
	static const char* const bundles[] = {
		"/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Arch, Alpine */
		"/etc/pki/tls/certs/ca-bundle.crt",   /* RHEL, Fedora */
		"/etc/ssl/ca-bundle.pem",             /* SUSE */
		NULL,
	};
	const char* ca = anyfs_tls_ca_pick(bundles);
	if (ca)
		setenv("SSL_CERT_FILE", ca, 0);
#endif
}
