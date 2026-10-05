/*
 * anyfs_ts.c — thin C glue between the JS side of @anyfs/core and the
 * existing libanyfs_core.a + liblkl.a stack.
 *
 * Design rule: one wasm call per logical operation. readdir/stat return
 * JSON to amortise wasm↔JS crossing cost (one call per directory rather
 * than per entry).
 *
 * Buffer-size protocol for the *_json helpers: if `cap` is too small,
 * returns a NEGATIVE number whose absolute value is the byte count we
 * would have written. JS retries with that buffer.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <pthread.h>
#endif

#include <lkl.h>
#include <lkl_host.h>

#include "anyfs.h"
#include "anyfs_probe.h"

#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

#include "jsonw.h"

/* ── Session table ──────────────────────────────────────── */
/* anyfs_session_open returns AnyfsSession* — opaque to JS. We give JS small
 * integer handles instead. */

#define MAX_HANDLES 8
static AnyfsSession* g_handles[MAX_HANDLES];

static int alloc_handle(AnyfsSession* d)
{
	for (int i = 0; i < MAX_HANDLES; i++) {
		if (!g_handles[i]) {
			g_handles[i] = d;
			return i;
		}
	}
	return -1;
}

static AnyfsSession* get_handle(int h)
{
	if (h < 0 || h >= MAX_HANDLES)
		return NULL;
	return g_handles[h];
}

/* ── Exported API ────────────────────────────────────────────── */

/* Route LKL printk through stderr instead of the default
 * emscripten_console_log. emscripten's stderr fd write is proxied to the
 * main thread, where Module.printErr (wired in worker.ts) captures the
 * message and forwards it as a `stderr` event to worker-client.ts; that
 * surfaces it as a `[anyfs.err]` console.log on the page. The default LKL
 * wasm print bypasses Module.printErr entirely (direct console.log from
 * the worker context), so it would only appear in the per-worker DevTools
 * console — invisible to CDP, untagged, and missed by host UIs that read
 * the page console. */
static void ts_lkl_print(const char* str, int len)
{
	fwrite(str, 1, len, stderr);
	fflush(stderr);
}

int anyfs_ts_kernel_init(uint32_t mem_mb, uint32_t loglevel)
{
	lkl_host_ops.print = ts_lkl_print;
	AnyfsKernelOpts opts = {.mem_mb = mem_mb, .loglevel = loglevel};
	return anyfs_kernel_init(&opts);
}

int anyfs_ts_kernel_halt(void)
{
	for (int i = 0; i < MAX_HANDLES; i++) {
		if (g_handles[i]) {
			anyfs_session_close(g_handles[i]);
			g_handles[i] = NULL;
		}
	}
	anyfs_kernel_halt();
	return 0;
}

int anyfs_ts_session_open(const char* image_path, uint32_t flags)
{
	AnyfsSession* d = NULL;
	int rc = anyfs_session_open(image_path, flags, &d);
	if (rc != 0 || !d)
		return -1;
	int h = alloc_handle(d);
	if (h < 0) {
		anyfs_session_close(d);
		return -2;
	}
	return h;
}

int anyfs_ts_session_close(int h)
{
	AnyfsSession* d = get_handle(h);
	if (!d)
		return -1;
	anyfs_session_close(d);
	g_handles[h] = NULL;
	return 0;
}

int anyfs_ts_session_list_json(int h, char* buf, size_t cap)
{
	AnyfsSession* d = get_handle(h);
	if (!d)
		return -1;
	AnyfsPartInfo parts[32];
	size_t got = 0;
	int n = anyfs_session_list(d, -1, parts, 32, &got);
	if (n < 0)
		return -2;
	if ((size_t)n > 32)
		n = 32;

	JsonW w;
	jw_init(&w, buf, cap);
	jw_putc(&w, '[');
	for (int i = 0; i < n; i++) {
		AnyfsPartInfo* p = &parts[i];
		if (i)
			jw_putc(&w, ',');
		jw_putc(&w, '{');
		jw_kv_int(&w, "slot_id", p->slot_id, 1);
		jw_kv_int(&w, "parent", p->parent, 1);
		jw_kv_int(&w, "index", (long long)p->index, 1);
		jw_kv_uint(&w, "offset", (unsigned long long)p->offset_bytes,
			   1);
		jw_kv_uint(&w, "size", (unsigned long long)p->size_bytes, 1);
		jw_kv_str(&w, "ptype", p->ptype, 1);
		jw_kv_str(&w, "kind", anyfs_partkind_name(p->kind), 1);
		jw_kv_str(&w, "fstype", p->fstype, 1);
		jw_kv_str(&w, "label", p->label, 1);
		jw_kv_str(&w, "uuid", p->uuid, 0);
		jw_putc(&w, '}');
	}
	jw_putc(&w, ']');
	return jw_finish(&w, buf, cap);
}

int anyfs_ts_session_meta_json(int h, char* buf, size_t cap)
{
	AnyfsSession* d = get_handle(h);
	if (!d)
		return -1;
	AnyfsSessionMeta m;
	if (anyfs_session_meta(d, &m) != 0)
		return -2;
	JsonW w;
	jw_init(&w, buf, cap);
	jw_putc(&w, '{');
	jw_kv_uint(&w, "logical_size", (unsigned long long)m.logical_size, 1);
	jw_kv_str(&w, "pt_type", m.pt_type, 0);
	jw_putc(&w, '}');
	return jw_finish(&w, buf, cap);
}

int anyfs_ts_session_enter(int h, unsigned int part, uint32_t flags,
			   char* mount_out, size_t mount_cap)
{
	AnyfsSession* d = get_handle(h);
	if (!d)
		return -1;
	if (mount_cap < 64)
		return -2;
	char lkl_path[64];
	/* The UI shows one mount at a time and has no "leave": entering the
	 * whole disk after one of its partitions (or the reverse) must
	 * replace that mount rather than fail with EBUSY. */
	int rc = anyfs_session_enter(d, part, flags | ANYFS_MOUNT_REPLACE,
				     lkl_path);
	if (rc != 0)
		return rc < 0 ? rc : -3;
	snprintf(mount_out, mount_cap, "%s", lkl_path);
	return 0;
}

int anyfs_ts_readdir_json(const char* path, char* buf, size_t cap)
{
	int err = 0;
	struct lkl_dir* dir = lkl_opendir(path, &err);
	if (!dir)
		return err < 0 ? err : -1;

	JsonW w;
	jw_init(&w, buf, cap);
	jw_putc(&w, '[');

	int first = 1;
	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir)) != NULL) {
		const char* name = de->d_name;
		if (name[0] == '.' &&
		    (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
			continue;

		if (!first)
			jw_putc(&w, ',');
		first = 0;

		const char* kind = "other";
		switch (de->d_type) {
		case DT_DIR:
			kind = "dir";
			break;
		case DT_REG:
			kind = "file";
			break;
		case DT_LNK:
			kind = "link";
			break;
		default:
			break;
		}

		jw_putc(&w, '{');
		jw_kv_str(&w, "name", name, 1);
		jw_kv_uint(&w, "ino", (unsigned long long)de->d_ino, 1);
		jw_kv_str(&w, "kind", kind, 0);
		jw_putc(&w, '}');
	}
	lkl_closedir(dir);
	jw_putc(&w, ']');
	return jw_finish(&w, buf, cap);
}

static int emit_stat_json(struct lkl_stat* st, char* buf, size_t cap)
{
	const char* kind = "other";
	unsigned int m = st->st_mode & 0170000;
	if (m == 0040000)
		kind = "dir";
	else if (m == 0100000)
		kind = "file";
	else if (m == 0120000)
		kind = "link";

	JsonW w;
	jw_init(&w, buf, cap);
	jw_putc(&w, '{');
	jw_kv_uint(&w, "ino", (unsigned long long)st->st_ino, 1);
	jw_kv_uint(&w, "mode", (unsigned long long)st->st_mode, 1);
	jw_kv_uint(&w, "size", (unsigned long long)st->st_size, 1);
	jw_kv_uint(&w, "nlink", (unsigned long long)st->st_nlink, 1);
	jw_kv_uint(&w, "uid", (unsigned long long)st->st_uid, 1);
	jw_kv_uint(&w, "gid", (unsigned long long)st->st_gid, 1);
	jw_kv_uint(&w, "dev", (unsigned long long)st->st_dev, 1);
	jw_kv_uint(&w, "rdev", (unsigned long long)st->st_rdev, 1);
	jw_kv_uint(&w, "blksize", (unsigned long long)st->st_blksize, 1);
	jw_kv_uint(&w, "blocks", (unsigned long long)st->st_blocks, 1);
	jw_kv_int(&w, "mtime", (long long)st->lkl_st_mtime, 1);
	jw_kv_int(&w, "atime", (long long)st->lkl_st_atime, 1);
	jw_kv_int(&w, "ctime", (long long)st->lkl_st_ctime, 1);
	jw_kv_str(&w, "kind", kind, 0);
	jw_putc(&w, '}');
	return jw_finish(&w, buf, cap);
}

int anyfs_ts_lstat_json(const char* path, char* buf, size_t cap)
{
	struct lkl_stat st;
	long rc = lkl_sys_lstat(path, &st);
	if (rc < 0)
		return (int)rc;
	return emit_stat_json(&st, buf, cap);
}

/* stat (follows symlinks). Needed by openReadable so the streamed size
 * reflects the target file, not the symlink's text length. */
int anyfs_ts_stat_json(const char* path, char* buf, size_t cap)
{
	struct lkl_stat st;
	long rc = lkl_sys_stat(path, &st);
	if (rc < 0)
		return (int)rc;
	return emit_stat_json(&st, buf, cap);
}

/* Canonicalize a directory path: follow all symlink hops and return the
 * absolute LKL path. Caller must already know `path` resolves to a dir
 * (chdir on a non-dir returns ENOTDIR). Uses chdir+getcwd because LKL has
 * no realpath syscall and procfs /proc/self/fd readlinks aren't reliably
 * shaped on this build. Worker.ts serialises ops, so the cwd mutation is
 * single-threaded and we restore the saved cwd before returning. */
int anyfs_ts_realpath(const char* path, char* buf, size_t cap)
{
	char saved[1024];
	long s = lkl_sys_getcwd(saved, sizeof(saved));
	if (s < 0)
		return (int)s;
	long c = lkl_sys_chdir(path);
	if (c < 0)
		return (int)c;
	long g = lkl_sys_getcwd(buf, cap);
	(void)lkl_sys_chdir(saved);
	if (g < 0)
		return (int)g;
	/* sys_getcwd returns length including NUL; report strlen to match
	 * the *_json convention (positive = bytes written, no NUL). */
	return (int)g - 1;
}

/* Read the verbatim target of a symlink. Returns bytes written (no NUL),
 * or negative errno (e.g. -EINVAL when `path` is not a symlink). */
int anyfs_ts_readlink(const char* path, char* buf, size_t cap)
{
	long n = lkl_sys_readlink(path, buf, cap);
	return (int)n;
}

/* Read a small text file from the LKL kernel namespace (e.g.
 * /proc/filesystems, /proc/mounts). Returns bytes written (no NUL)
 * or negative errno. */
int anyfs_ts_read_kernel_file(const char* path, char* buf, size_t cap)
{
	long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
	if (fd < 0)
		return (int)fd;
	long n = lkl_sys_read(fd, buf, cap - 1);
	lkl_sys_close(fd);
	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	return (int)n;
}

int anyfs_ts_open(const char* path, int flags)
{
	long fd = lkl_sys_open(path, flags, 0);
	return (int)fd;
}

int64_t anyfs_ts_pread(int fd, void* buf, uint32_t n, int64_t off)
{
	return (int64_t)lkl_sys_pread64((unsigned int)fd, (char*)buf,
					(lkl_size_t)n, (lkl_loff_t)off);
}

int anyfs_ts_close(int fd)
{
	return (int)lkl_sys_close(fd);
}

#ifdef __EMSCRIPTEN__
/* ── API thread (wasm) ─────────────────────────────────────────────────
 *
 * Every op runs on one long-lived pthread. The thread that owns the module
 * (the browser Worker, or Node's main thread) must never block: pthreads
 * proxy their file-system calls to it — including the QEMU thread's reads
 * of the image through WORKERFS / URLFS / NODEFS — and only it can start
 * the Workers that new pthreads (kernel threads during boot or an ext4 /
 * btrfs mount) need. So JS fills a struct anyfs_ts_req in linear memory,
 * anyfs_ts_api_submit() queues it and returns at once, and the API thread
 * reports completion on the owning thread through Module.anyfsApiDone(id).
 *
 * The op numbers and the struct layout are mirrored in
 * ts/packages/core/src/wasm-api.ts (and the bundle smoke test).
 */
enum {
	ANYFS_TS_OP_KERNEL_INIT = 1,	   /* mem_mb, loglevel */
	ANYFS_TS_OP_KERNEL_HALT = 2,	   /* — */
	ANYFS_TS_OP_SESSION_OPEN = 3,	   /* path, flags */
	ANYFS_TS_OP_SESSION_CLOSE = 4,	   /* h */
	ANYFS_TS_OP_SESSION_LIST = 5,	   /* h, buf, cap */
	ANYFS_TS_OP_SESSION_META = 6,	   /* h, buf, cap */
	ANYFS_TS_OP_SESSION_ENTER = 7,	   /* h, part, flags, buf, cap */
	ANYFS_TS_OP_READDIR = 8,	   /* path, buf, cap */
	ANYFS_TS_OP_LSTAT = 9,		   /* path, buf, cap */
	ANYFS_TS_OP_STAT = 10,		   /* path, buf, cap */
	ANYFS_TS_OP_REALPATH = 11,	   /* path, buf, cap */
	ANYFS_TS_OP_READLINK = 12,	   /* path, buf, cap */
	ANYFS_TS_OP_READ_KERNEL_FILE = 13, /* path, buf, cap */
	ANYFS_TS_OP_OPEN = 14,		   /* path, flags */
	ANYFS_TS_OP_PREAD = 15,		   /* fd, buf, n, off_lo, off_hi */
	ANYFS_TS_OP_CLOSE = 16,		   /* fd */
	ANYFS_TS_OP_LAST_ERROR = 17,	   /* buf, cap */
};

struct anyfs_ts_req {
	int32_t op;  /* ANYFS_TS_OP_* */
	int32_t id;  /* JS correlation id, echoed to anyfsApiDone */
	int32_t ret; /* result, valid once anyfsApiDone(id) ran */
	int32_t arg[6];
	struct anyfs_ts_req* next; /* queue link, owned by C */
};

static pthread_mutex_t g_api_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_api_cond = PTHREAD_COND_INITIALIZER;
static struct anyfs_ts_req *g_api_head, *g_api_tail;
static int g_api_started;

/* Copy this thread's last error (the API thread's — it ran the failed op)
 * into buf. Returns its length, or 0 if there is none. */
static int api_last_error(char* buf, size_t cap)
{
	const char* e = anyfs_get_last_error();
	if (!cap)
		return 0;
	snprintf(buf, cap, "%s", e ? e : "");
	return (int)strlen(buf);
}

static int32_t api_run(const struct anyfs_ts_req* r)
{
	const int32_t* a = r->arg;
#define P(i) ((void*)(intptr_t)a[i])
#define S(i) ((const char*)(intptr_t)a[i])
#define Z(i) ((size_t)(uint32_t)a[i])
	switch (r->op) {
	case ANYFS_TS_OP_KERNEL_INIT:
		return anyfs_ts_kernel_init((uint32_t)a[0], (uint32_t)a[1]);
	case ANYFS_TS_OP_KERNEL_HALT:
		return anyfs_ts_kernel_halt();
	case ANYFS_TS_OP_SESSION_OPEN:
		anyfs_set_last_error("%s", "");
		return anyfs_ts_session_open(S(0), (uint32_t)a[1]);
	case ANYFS_TS_OP_SESSION_CLOSE:
		return anyfs_ts_session_close(a[0]);
	case ANYFS_TS_OP_SESSION_LIST:
		return anyfs_ts_session_list_json(a[0], P(1), Z(2));
	case ANYFS_TS_OP_SESSION_META:
		return anyfs_ts_session_meta_json(a[0], P(1), Z(2));
	case ANYFS_TS_OP_SESSION_ENTER:
		anyfs_set_last_error("%s", "");
		return anyfs_ts_session_enter(a[0], (unsigned int)a[1],
					      (uint32_t)a[2], P(3), Z(4));
	case ANYFS_TS_OP_READDIR:
		return anyfs_ts_readdir_json(S(0), P(1), Z(2));
	case ANYFS_TS_OP_LSTAT:
		return anyfs_ts_lstat_json(S(0), P(1), Z(2));
	case ANYFS_TS_OP_STAT:
		return anyfs_ts_stat_json(S(0), P(1), Z(2));
	case ANYFS_TS_OP_REALPATH:
		return anyfs_ts_realpath(S(0), P(1), Z(2));
	case ANYFS_TS_OP_READLINK:
		return anyfs_ts_readlink(S(0), P(1), Z(2));
	case ANYFS_TS_OP_READ_KERNEL_FILE:
		return anyfs_ts_read_kernel_file(S(0), P(1), Z(2));
	case ANYFS_TS_OP_OPEN:
		return anyfs_ts_open(S(0), a[1]);
	case ANYFS_TS_OP_PREAD: {
		int64_t off = (int64_t)(((uint64_t)(uint32_t)a[4] << 32) |
					(uint32_t)a[3]);
		int64_t got = anyfs_ts_pread(a[0], P(1), (uint32_t)a[2], off);
		return got > INT32_MAX ? INT32_MAX : (int32_t)got;
	}
	case ANYFS_TS_OP_CLOSE:
		return anyfs_ts_close(a[0]);
	case ANYFS_TS_OP_LAST_ERROR:
		return api_last_error(P(0), Z(1));
	}
#undef P
#undef S
#undef Z
	return -1;
}

static void* api_thread_fn(void* unused)
{
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&g_api_lock);
		while (!g_api_head)
			pthread_cond_wait(&g_api_cond, &g_api_lock);
		struct anyfs_ts_req* r = g_api_head;
		g_api_head = r->next;
		if (!g_api_head)
			g_api_tail = NULL;
		pthread_mutex_unlock(&g_api_lock);

		r->ret = api_run(r);
		MAIN_THREAD_ASYNC_EM_ASM(
		    { Module["anyfsApiDone"]($0); }, r->id);
	}
	return NULL;
}

/* Queue `r` for the API thread (started on first use) and return at once.
 * Called on the module-owning thread. Returns 0, or -1 if the thread could
 * not be started. */
int anyfs_ts_api_submit(struct anyfs_ts_req* r)
{
	pthread_mutex_lock(&g_api_lock);
	if (!g_api_started) {
		pthread_t t;
		if (pthread_create(&t, NULL, api_thread_fn, NULL) != 0) {
			pthread_mutex_unlock(&g_api_lock);
			return -1;
		}
		pthread_detach(t);
		g_api_started = 1;
	}
	r->next = NULL;
	if (g_api_tail)
		g_api_tail->next = r;
	else
		g_api_head = r;
	g_api_tail = r;
	pthread_cond_signal(&g_api_cond);
	pthread_mutex_unlock(&g_api_lock);
	return 0;
}
#endif /* __EMSCRIPTEN__ */

/* PROXY_TO_PTHREAD requires a main(). It has nothing to do: every op runs
 * on the API thread (anyfs_ts_api_submit). emscripten_exit_with_live_runtime
 * returns the pthread to its event loop without tearing the runtime down. */
int main(void)
{
#ifdef __EMSCRIPTEN__
	emscripten_exit_with_live_runtime();
#endif
	return 0;
}
