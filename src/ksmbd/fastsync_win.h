/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ANYFS_FASTSYNC_WIN_H
#define ANYFS_FASTSYNC_WIN_H

/*
 * Override lkl_host_ops sem and mutex ops with WaitOnAddress + CRITICAL_SECTION
 * implementations. MUST be called before lkl_init(&lkl_host_ops).
 * No-op on non-Windows builds.
 */
void lkl_fastsync_install(void);

#endif
