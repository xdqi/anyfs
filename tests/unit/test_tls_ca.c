// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the CA-bundle pick in src/core/anyfs_tls.c. */
#include "anyfs_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
			failures++;                                          \
		}                                                            \
	} while (0)

int main(void)
{
	char present[] = "/tmp/anyfs-tls-ca-XXXXXX";
	int fd = mkstemp(present);
	CHECK(fd >= 0);
	close(fd);
	const char* const cands[] = {"/nonexistent/anyfs-ca.crt", present,
				     NULL};

	unsetenv("SSL_CERT_FILE");
	const char* got = anyfs_tls_ca_pick(cands);
	CHECK(got && strcmp(got, present) == 0);

	/* A user-supplied SSL_CERT_FILE always wins. */
	setenv("SSL_CERT_FILE", "/elsewhere.pem", 1);
	CHECK(anyfs_tls_ca_pick(cands) == NULL);
	unsetenv("SSL_CERT_FILE");

	const char* const none[] = {"/nonexistent/a", "/nonexistent/b", NULL};
	CHECK(anyfs_tls_ca_pick(none) == NULL);

	unlink(present);
	if (failures)
		return 1;
	printf("tls_ca: all checks passed\n");
	return 0;
}
