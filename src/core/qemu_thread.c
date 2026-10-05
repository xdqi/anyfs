/*
 * qemu_thread.c — the dedicated QEMU thread. See qemu_thread.h.
 *
 * Hand-off: callers schedule a one-shot bottom half on the QEMU thread's
 * AioContext (aio_bh_schedule_oneshot is QEMU's thread-safe way in) and wait
 * on a per-call semaphore. The BH either runs the function directly or
 * starts a coroutine for it, so I/O never nests aio_poll() inside a BH.
 *
 * Every wait is bounded by a watchdog (ANYFS_QEMU_TIMEOUT_MS, default
 * 120 s). On timeout the waiter reports anyfs_fatal() and keeps waiting: the
 * QEMU side may still be writing into the caller's buffers (LKL page
 * memory), so returning early could corrupt memory later.
 */
/* qemu/osdep.h MUST be the first include in every QEMU-using .c file. */
#include "qemu/osdep.h"

#include <stdio.h>
#include <stdlib.h>

#include "block/block-global-state.h"
#include "block/thread-pool.h"
#include "qapi/error.h"
#include "qemu/aio.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"

/* anyfs_kernel.h pulls in lkl_host.h, whose struct iovec clashes with
 * QEMU's on Windows — same workaround as qemu_backend.c. */
#ifdef _WIN32
#define __MSYS__ 1
#endif
#include "anyfs_kernel.h"
#ifdef _WIN32
#undef __MSYS__
#endif
#include "qemu_thread.h"

#define QEMU_THREAD_DEFAULT_TIMEOUT_MS 120000

enum qt_state {
	QT_IDLE,    /* never started */
	QT_RUNNING, /* accepting calls */
	QT_FAILED,  /* start failed (sticky) */
	QT_STOPPED, /* stopped (sticky) */
};

static GMutex g_start_lock; /* zero-initialised static GMutex is valid */
static int g_state = QT_IDLE;
static char g_start_err[256];
static QemuThread g_thread;
static QemuSemaphore g_ready;
static QemuSemaphore g_stopped;
static AioContext* g_ctx;
static bool g_stop_requested;
static __thread bool t_on_qemu_thread;

static int timeout_ms(void)
{
	static int cached;
	int v = qatomic_read(&cached);
	if (v > 0)
		return v;
	const char* env = getenv("ANYFS_QEMU_TIMEOUT_MS");
	long parsed = env ? strtol(env, NULL, 10) : 0;
	v = parsed > 0 && parsed < INT_MAX ? (int)parsed
					   : QEMU_THREAD_DEFAULT_TIMEOUT_MS;
	qatomic_set(&cached, v);
	return v;
}

/* Block until sem is posted. Past the watchdog timeout, report a fatal error
 * once and keep waiting (see the file comment for why we never bail out). */
static void wait_done(QemuSemaphore* sem, const char* what)
{
	int ms = timeout_ms();
	if (qemu_sem_timedwait(sem, ms) == 0)
		return;
	anyfs_fatal("QEMU thread did not finish %s within %d ms", what, ms);
	qemu_sem_wait(sem);
}

bool qemu_thread_is_current(void)
{
	return t_on_qemu_thread;
}

static void* qemu_thread_fn(void* arg)
{
	Error* err = NULL;
	GMainContext* gctx;
	(void)arg;

	t_on_qemu_thread = true;
	rcu_register_thread();

	/* LKL has one CPU, so at most one block request is in flight. Handing
	 * file I/O and qcow2 (de)compression to QEMU's worker pool would add
	 * two more cross-thread wake-ups per request for no parallelism, and
	 * under emscripten a worker's reply inside an Asyncify rewind aborts
	 * the runtime. Run that work inline on this thread (patch 0011). */
	thread_pool_set_inline(true);

	/* Keep every QEMU GSource off the global default GMainContext, which a
	 * host GUI loop (Electron/Chromium on Linux) may be iterating. */
	gctx = g_main_context_new();
	g_main_context_push_thread_default(gctx);
	qemu_main_loop_set_gcontext(gctx);

	/* MODULE_INIT_QOM must precede bdrv_init so QOM types such as
	 * QIOChannelSocket (NBD) are registered; qemu-img/qemu-nbd do the
	 * same. */
	module_call_init(MODULE_INIT_QOM);
	bdrv_init();
	if (qemu_init_main_loop(&err) < 0) {
		snprintf(g_start_err, sizeof(g_start_err),
			 "qemu_init_main_loop failed: %s",
			 err ? error_get_pretty(err) : "unknown error");
		error_free(err);
		qatomic_set(&g_state, QT_FAILED);
		rcu_unregister_thread();
		qemu_sem_post(&g_ready);
		return NULL;
	}
	g_ctx = qemu_get_aio_context();
	qatomic_set(&g_state, QT_RUNNING);
	qemu_sem_post(&g_ready);

	while (!qatomic_read(&g_stop_requested))
		aio_poll(g_ctx, true);

	rcu_unregister_thread();
	qemu_sem_post(&g_stopped);
	return NULL;
}

int qemu_thread_start(char* err, size_t err_cap)
{
	int state;

	assert(!t_on_qemu_thread);
	g_mutex_lock(&g_start_lock);
	if (g_state == QT_IDLE) {
		qemu_sem_init(&g_ready, 0);
		qemu_sem_init(&g_stopped, 0);
		/* Detached: shutdown waits on g_stopped instead of a join.
		 * Under emscripten a pthread whose stack Asyncify has unwound
		 * for a coroutine switch never reaches the thread-exit path
		 * that a join waits for. */
		qemu_thread_create(&g_thread, "anyfs-qemu", qemu_thread_fn,
				   NULL, QEMU_THREAD_DETACHED);
		wait_done(&g_ready, "initialisation");
	}
	state = qatomic_read(&g_state);
	g_mutex_unlock(&g_start_lock);

	if (state == QT_RUNNING)
		return 0;
	snprintf(err, err_cap, "%s",
		 state == QT_STOPPED ? "QEMU thread already stopped"
				     : g_start_err);
	return -1;
}

struct qt_call {
	void (*fn)(void* opaque);
	CoroutineEntry* co_fn;
	void* opaque;
	QemuSemaphore done;
};

static void coroutine_fn qt_call_co(void* opaque)
{
	struct qt_call* c = opaque;
	c->co_fn(c->opaque);
	qemu_sem_post(&c->done);
}

static void qt_call_bh(void* opaque)
{
	struct qt_call* c = opaque;

	if (c->co_fn) {
		qemu_coroutine_enter(qemu_coroutine_create(qt_call_co, c));
		return;
	}
	c->fn(c->opaque);
	qemu_sem_post(&c->done);
}

static int run_on_thread(struct qt_call* c, const char* what)
{
	assert(!t_on_qemu_thread);
	if (qatomic_read(&g_state) != QT_RUNNING)
		return -1;
	qemu_sem_init(&c->done, 0);
	aio_bh_schedule_oneshot(g_ctx, qt_call_bh, c);
	wait_done(&c->done, what);
	qemu_sem_destroy(&c->done);
	return 0;
}

int qemu_thread_call(void (*fn)(void* opaque), void* opaque)
{
	struct qt_call c = {.fn = fn, .opaque = opaque};
	return run_on_thread(&c, "a call");
}

int qemu_thread_co_call(CoroutineEntry* fn, void* opaque)
{
	struct qt_call c = {.co_fn = fn, .opaque = opaque};
	return run_on_thread(&c, "an I/O request");
}

static void qt_stop_bh(void* opaque)
{
	(void)opaque;
	qatomic_set(&g_stop_requested, true);
}

void qemu_thread_stop(void)
{
	assert(!t_on_qemu_thread);
	g_mutex_lock(&g_start_lock);
	if (g_state == QT_RUNNING) {
		qatomic_set(&g_state, QT_STOPPED);
		aio_bh_schedule_oneshot(g_ctx, qt_stop_bh, NULL);
		wait_done(&g_stopped, "shutdown");
	} else if (g_state == QT_IDLE) {
		g_state = QT_STOPPED;
	}
	g_mutex_unlock(&g_start_lock);
}
