/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ANYFS_LKL_FASTSYNC_H
#define ANYFS_LKL_FASTSYNC_H

/*
 * Override lkl_host_ops sem and mutex ops with WaitOnAddress + CRITICAL_SECTION
 * implementations. MUST be called before lkl_init(&lkl_host_ops).
 * No-op on non-Windows builds.
 */
void lkl_fastsync_install(void);

#endif
