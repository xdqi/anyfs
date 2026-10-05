/*
 * qemu_thread.h — the dedicated QEMU thread (internal).
 *
 * QEMU's block layer assumes one home thread owns and runs its main
 * AioContext. anyfs gives it exactly that: one thread per process (or wasm
 * instance) initialises QEMU, owns the AioContext — attached to a private
 * GMainContext, never the global default one — and loops on aio_poll().
 * Every other thread (LKL kernel threads, the Electron main thread, libuv
 * workers, CLI main threads) only posts work to it and blocks until done.
 *
 * Rules (asserted):
 *   - Only the QEMU thread calls QEMU functions.
 *   - The QEMU thread never calls lkl_* and never calls qemu_thread_call /
 *     qemu_thread_co_call (it would wait on itself).
 *
 * Callers include "qemu/osdep.h" first, as for any QEMU-using file.
 * See docs/superpowers/specs/2026-10-05-qemu-dedicated-thread-design.md.
 */
#ifndef ANYFS_QEMU_THREAD_H
#define ANYFS_QEMU_THREAD_H

#include "qemu/coroutine.h"

/* Start the QEMU thread if it is not running and wait for it to finish
 * initialising. Idempotent and thread-safe. A start failure is sticky.
 * Returns 0, or -1 with a message in err. */
int qemu_thread_start(char* err, size_t err_cap);

/* Run fn(opaque) on the QEMU thread as a bottom half and block until it
 * returns. For global-state code (blk_new_open, blk_unref). Returns 0, or
 * -1 if the thread is not running. */
int qemu_thread_call(void (*fn)(void* opaque), void* opaque);

/* Run fn(opaque) as a coroutine on the QEMU thread and block until it
 * returns. For I/O code (blk_co_*). Returns 0, or -1 if the thread is not
 * running. */
int qemu_thread_co_call(CoroutineEntry* fn, void* opaque);

/* Stop the loop and join the thread. Later calls fail fast. */
void qemu_thread_stop(void);

/* True when called on the QEMU thread. */
bool qemu_thread_is_current(void);

#endif /* ANYFS_QEMU_THREAD_H */
