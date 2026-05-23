/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lkl_fastsync.c — Windows-only fast replacement for LKL host sem/mutex ops.
 *
 * The stock LKL nt-host.c uses CreateSemaphore + WaitForSingleObject for both
 * sem and mutex. Under wine each WaitForSingleObject / ReleaseSemaphore on a
 * kernel HANDLE round-trips through wineserver IPC (~tens of µs). LKL hits
 * these calls thousands of times per request (ksmbd schedule + per-mutex),
 * which dominates the wine-side throughput cost.
 *
 * This file installs faster implementations into the *global* `lkl_host_ops`
 * struct (a non-const, exported global) before lkl_init() runs:
 *
 *   - semaphores: int32 counter + WaitOnAddress/WakeByAddressSingle.
 *     User-mode fast path; wine's WaitOnAddress is a thin shim over a futex.
 *   - mutex: CRITICAL_SECTION (user-mode uncontended; wineserver only on
 *     contention). CRITICAL_SECTION is recursive — fine for both modes since
 *     LKL never relies on the kernel detecting a non-recursive double-lock.
 *
 * We do NOT touch ~/linux/tools/lkl/lib/nt-host.c — the override is a runtime
 * pointer swap. Other ops (threads, TLS, timers) keep the stock impl.
 *
 * Forward declarations in <uapi/asm/host_ops.h> declare `struct lkl_sem` and
 * `struct lkl_mutex` as opaque; the layouts in nt-host.c are private to it,
 * so we are free to define our own here — the kernel only ever round-trips
 * the pointers we hand back through sem_* and mutex_* ops.
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <stdint.h>
#include <stdlib.h>
#include <synchapi.h>
#include <windows.h>
#include <winsock2.h>

#include <lkl_host.h>

/* WaitOnAddress / WakeByAddressSingle live in API set
 * api-ms-win-core-synch-l1-2-0 → on mingw-w64 link with -lsynchronization. */

struct lkl_sem {
	/* Signed so we can detect negative values (waiters waiting). */
	volatile LONG count;
};

struct lkl_mutex {
	CRITICAL_SECTION cs;
};

/* ─── semaphore ────────────────────────────────────────────────────── */

static struct lkl_sem* fs_sem_alloc(int count)
{
	struct lkl_sem* s = malloc(sizeof(*s));
	if (!s)
		return NULL;
	s->count = count;
	return s;
}

static void fs_sem_free(struct lkl_sem* s)
{
	free(s);
}

static void fs_sem_up(struct lkl_sem* s)
{
	/* Bump the count, then wake one waiter. We always wake (cheap on the
	 * uncontended path: wine WakeByAddressSingle short-circuits if nobody
	 * is waiting on the address). */
	InterlockedIncrement(&s->count);
	WakeByAddressSingle((PVOID)&s->count);
}

static void fs_sem_down(struct lkl_sem* s)
{
	for (;;) {
		LONG cur = s->count;
		if (cur > 0) {
			if (InterlockedCompareExchange(&s->count, cur - 1,
						       cur) == cur)
				return;
			/* CAS lost — retry. */
			continue;
		}
		/* cur <= 0 — wait until somebody bumps it. The compared value
		 * is `cur`; we wake whenever the address changes away from
		 * `cur`. */
		WaitOnAddress((PVOID)&s->count, &cur, sizeof(cur), INFINITE);
	}
}

/* ─── mutex ────────────────────────────────────────────────────────── */

static struct lkl_mutex* fs_mutex_alloc(int recursive)
{
	(void)recursive; /* CRITICAL_SECTION is always recursive — safe either
			    way. */
	struct lkl_mutex* m = malloc(sizeof(*m));
	if (!m)
		return NULL;
	InitializeCriticalSection(&m->cs);
	return m;
}

static void fs_mutex_free(struct lkl_mutex* m)
{
	DeleteCriticalSection(&m->cs);
	free(m);
}

static void fs_mutex_lock(struct lkl_mutex* m)
{
	EnterCriticalSection(&m->cs);
}

static void fs_mutex_unlock(struct lkl_mutex* m)
{
	LeaveCriticalSection(&m->cs);
}

/* ─── installer ────────────────────────────────────────────────────── */

void lkl_fastsync_install(void)
{
	lkl_host_ops.sem_alloc = fs_sem_alloc;
	lkl_host_ops.sem_free = fs_sem_free;
	lkl_host_ops.sem_up = fs_sem_up;
	lkl_host_ops.sem_down = fs_sem_down;

	lkl_host_ops.mutex_alloc = fs_mutex_alloc;
	lkl_host_ops.mutex_free = fs_mutex_free;
	lkl_host_ops.mutex_lock = fs_mutex_lock;
	lkl_host_ops.mutex_unlock = fs_mutex_unlock;
}

#else /* !_WIN32 */

/* POSIX builds keep the stock nt-host? no — they use posix-host. Either way
 * this file is a no-op outside Windows. */
void lkl_fastsync_install(void)
{
}

#endif
