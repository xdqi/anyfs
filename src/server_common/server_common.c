// SPDX-License-Identifier: GPL-2.0-or-later
#include "server_common.h"

#include <lkl.h>
#include <stdio.h>

volatile sig_atomic_t anyfs_server_running = 1;

static void handle_stop(int sig)
{
	(void)sig;
	anyfs_server_running = 0;
}

void anyfs_server_install_signals(void)
{
	setbuf(stdout, NULL);
	signal(SIGINT, handle_stop);
	signal(SIGTERM, handle_stop);
}

int anyfs_server_boot(const AnyfsKernelOpts* opts)
{
	int ret = anyfs_kernel_init(opts);
	if (ret)
		return ret;
	/* lo is auto-up after boot, but the call is idempotent and
	 * documents intent. */
	lkl_if_up(1);
	return 0;
}

int anyfs_server_resolve_shares(char* const* specs, int n_specs,
				AnyfsSession** disks, int n_disks,
				uint32_t enter_flags, AnyfsShareEntry* out,
				int max_out)
{
	int n = 0;

	for (int si = 0; si < n_specs; si++) {
		if (n >= max_out) {
			fprintf(stderr, "error: too many shares (max %d)\n",
				max_out);
			return -1;
		}
		AnyfsShareEntry* e = &out[n];
		if (anyfs_share_resolve(specs[si], disks, n_disks, enter_flags,
					e->name, sizeof(e->name), e->lkl_path,
					sizeof(e->lkl_path)) < 0)
			return -1;
		n++;
	}
	return n;
}

void anyfs_server_shutdown(AnyfsSession** disks, int n_disks)
{
	for (int i = 0; i < n_disks; i++) {
		if (disks[i])
			anyfs_session_close(disks[i]);
	}
	anyfs_kernel_halt();
}
