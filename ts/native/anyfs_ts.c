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
#include "anyfs_disk.h"
#include "kindprobe.h"

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

/* ── Disk session table ──────────────────────────────────────── */
/* anyfs_disk_open returns AnyfsDisk* — opaque to JS. We give JS small
 * integer handles instead. */

#define MAX_HANDLES 8
static AnyfsDisk* g_handles[MAX_HANDLES];

static int alloc_handle(AnyfsDisk* d)
{
	for (int i = 0; i < MAX_HANDLES; i++) {
		if (!g_handles[i]) {
			g_handles[i] = d;
			return i;
		}
	}
	return -1;
}

static AnyfsDisk* get_handle(int h)
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

int anyfs_ts_init(uint32_t mem_mb, uint32_t loglevel)
{
	lkl_host_ops.print = ts_lkl_print;
	AnyfsKernelOpts opts = {.mem_mb = mem_mb, .loglevel = loglevel};
	return anyfs_kernel_init(&opts);
}

#ifdef __EMSCRIPTEN__
/*
 * Async boot for wasm/Worker environments: the synchronous anyfs_ts_init
 * blocks the calling thread in sem_down(init_sem) → Atomics.wait. While
 * the caller is blocked, pthread Workers cannot proxy nested thread
 * creation back to the calling thread (which owns the pool), so the
 * kernel deadlocks during boot.
 *
 * The fix: run anyfs_ts_init on a dedicated pthread. The pool-owning
 * thread returns to JavaScript immediately and polls
 * anyfs_ts_is_boot_complete(), with its event loop free to process
 * spawnThread messages while the kernel boots.
 */

static volatile int g_boot_complete = 0;
static volatile int g_boot_result = 0;

static void* boot_thread_fn(void* arg)
{
	uint32_t mem_mb = ((uint32_t*)arg)[0];
	uint32_t loglevel = ((uint32_t*)arg)[1];

	g_boot_result = anyfs_ts_init(mem_mb, loglevel);
	__sync_synchronize();
	g_boot_complete = 1;

	return NULL;
}

int anyfs_ts_init_async(uint32_t mem_mb, uint32_t loglevel)
{
	static uint32_t args[2];
	args[0] = mem_mb;
	args[1] = loglevel;

	pthread_t thread;
	int rc = pthread_create(&thread, NULL, boot_thread_fn, args);
	if (rc != 0)
		return -1;
	pthread_detach(thread);
	return 0; /* returns immediately — caller must poll */
}

int anyfs_ts_is_boot_complete(void)
{
	return g_boot_complete;
}

int anyfs_ts_boot_result(void)
{
	return g_boot_result;
}
#endif

int anyfs_ts_kernel_halt(void)
{
	for (int i = 0; i < MAX_HANDLES; i++) {
		if (g_handles[i]) {
			anyfs_disk_close(g_handles[i]);
			g_handles[i] = NULL;
		}
	}
	anyfs_kernel_halt();
	return 0;
}

int anyfs_ts_disk_open(const char* image_path, uint32_t flags)
{
	AnyfsDisk* d = NULL;
	int rc = anyfs_disk_open(image_path, flags, &d);
	if (rc != 0 || !d)
		return -1;
	int h = alloc_handle(d);
	if (h < 0) {
		anyfs_disk_close(d);
		return -2;
	}
	return h;
}

int anyfs_ts_disk_close(int h)
{
	AnyfsDisk* d = get_handle(h);
	if (!d)
		return -1;
	anyfs_disk_close(d);
	g_handles[h] = NULL;
	return 0;
}

int anyfs_ts_disk_list_json(int h, char* buf, size_t cap)
{
	AnyfsDisk* d = get_handle(h);
	if (!d)
		return -1;
	AnyfsPartInfo parts[32];
	size_t got = 0;
	int n = anyfs_disk_list(d, parts, 32, &got);
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

int anyfs_ts_disk_meta_json(int h, char* buf, size_t cap)
{
	AnyfsDisk* d = get_handle(h);
	if (!d)
		return -1;
	AnyfsDiskMeta m;
	if (anyfs_disk_meta(d, &m) != 0)
		return -2;
	JsonW w;
	jw_init(&w, buf, cap);
	jw_putc(&w, '{');
	jw_kv_uint(&w, "logical_size", (unsigned long long)m.logical_size, 1);
	jw_kv_str(&w, "pt_type", m.pt_type, 0);
	jw_putc(&w, '}');
	return jw_finish(&w, buf, cap);
}

int anyfs_ts_disk_enter(int h, unsigned int part, uint32_t flags,
			char* mount_out, size_t mount_cap)
{
	AnyfsDisk* d = get_handle(h);
	if (!d)
		return -1;
	if (mount_cap < 64)
		return -2;
	char lkl_path[64];
	int rc = anyfs_disk_enter(d, part, flags, lkl_path);
	if (rc != 0)
		return rc < 0 ? rc : -3;
	snprintf(mount_out, mount_cap, "%s", lkl_path);
	return 0;
}

/* Mount the whole disk (no partition table) by creating a /dev node
 * for /dev/anyfs_d<id>_whole and calling anyfs_mount_blkdev.
 *
 * Dev number and fstype hint are cached from anyfs_disk_open to avoid
 * QEMU block I/O in this call — any pread64 through QEMU fibers before
 * the mount would corrupt ASYNCIFY state and wedge the ext4 mount. */
int anyfs_ts_mount_whole(int h, const char* fstype, uint32_t flags,
			 char* mount_out, size_t mount_cap)
{
	AnyfsDisk* d = get_handle(h);
	if (!d)
		return -1;
	if (mount_cap < 64)
		return -2;
	int disk_id = anyfs_disk_id(d);
	if (disk_id < 0)
		return -3;

	/* Use cached dev_t from anyfs_disk_open; fall back to sysfs read
	 * if the cache is cold (shouldn't happen, but be defensive). */
	uint32_t dev = anyfs_disk_whole_dev(d);
	if (dev == 0) {
		if (lkl_get_virtio_blkdev(disk_id, 0, &dev) < 0)
			return -4;
	}

	if (lkl_sys_access("/dev", 0) < 0)
		lkl_sys_mkdir("/dev", 0700);
	char devpath[80];
	snprintf(devpath, sizeof(devpath), "/dev/anyfs_d%d_whole", disk_id);
	(void)lkl_sys_mknod(devpath, LKL_S_IFBLK | 0600, dev);

	char name[32];
	snprintf(name, sizeof(name), "anyfs_d%d_whole", disk_id);

	/* Resolve fstype: explicit arg > cached superblock probe > kindprobe.
	 * The inline pread64 probe is intentionally absent here — any QEMU
	 * block I/O before the mount corrupts ASYNCIFY fiber state. */
	const char* hint =
	    (fstype && *fstype && strcmp(fstype, "auto") != 0) ? fstype : NULL;
	if (!hint)
		hint = anyfs_disk_whole_fstype_hint(d);
	if (!hint) {
		char probed[32] = {0}, lbl[64] = {0}, uid[40] = {0};
		(void)anyfs_kindprobe_meta(devpath, probed, lbl, uid);
		if (probed[0])
			hint = probed;
	}

	/* Flush ASYNCIFY fiber state before the ext4 mount's block I/O.
	 * fprintf+fflush to stderr proxies to the main thread via
	 * postMessage+Atomics.wait, forcing a full save/restore cycle. */
#ifdef __EMSCRIPTEN__
	fprintf(stderr, " ");
	fflush(stderr);
#endif

	AnyfsMount mnt = {0};
	int rc = anyfs_mount_blkdev(devpath, hint, name, flags, &mnt);
	if (rc != 0)
		return rc < 0 ? rc : -5;
	snprintf(mount_out, mount_cap, "%s", mnt.mount_point);
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

/* ── Out-pointer variants for ASYNCIFY+fiber compatibility ─────────────
 *
 * When the wasm bundle uses ASYNCIFY=1 and QEMU's coroutine backend
 * (emscripten_fiber), every blk_pread does a fiber_swap which forces an
 * asyncify unwind/rewind cycle. Emscripten's `Fibers.finishContextSwitch`
 * discards the wasm export's return value (it's only preserved on the
 * handleSleep path). Workaround: write the result through an out-pointer,
 * JS reads it via getValue/HEAP32 after the call resolves. */

void anyfs_ts_disk_open_p(const char* image_path, uint32_t flags, int32_t* out)
{
	*out = anyfs_ts_disk_open(image_path, flags);
}

void anyfs_ts_disk_list_json_p(int h, char* buf, size_t cap, int32_t* out)
{
	*out = anyfs_ts_disk_list_json(h, buf, cap);
}

void anyfs_ts_disk_meta_json_p(int h, char* buf, size_t cap, int32_t* out)
{
	*out = anyfs_ts_disk_meta_json(h, buf, cap);
}

void anyfs_ts_mount_whole_p(int h, const char* fstype, uint32_t flags,
			    char* mount_out, size_t mount_cap, int32_t* out)
{
	*out = anyfs_ts_mount_whole(h, fstype, flags, mount_out, mount_cap);
}

void anyfs_ts_disk_enter_p(int h, unsigned int part, uint32_t flags,
			   char* mount_out, size_t mount_cap, int32_t* out)
{
	*out = anyfs_ts_disk_enter(h, part, flags, mount_out, mount_cap);
}

void anyfs_ts_readdir_json_p(const char* path, char* buf, size_t cap,
			     int32_t* out)
{
	*out = anyfs_ts_readdir_json(path, buf, cap);
}

void anyfs_ts_lstat_json_p(const char* path, char* buf, size_t cap,
			   int32_t* out)
{
	*out = anyfs_ts_lstat_json(path, buf, cap);
}

void anyfs_ts_stat_json_p(const char* path, char* buf, size_t cap, int32_t* out)
{
	*out = anyfs_ts_stat_json(path, buf, cap);
}

void anyfs_ts_realpath_p(const char* path, char* buf, size_t cap, int32_t* out)
{
	*out = anyfs_ts_realpath(path, buf, cap);
}

void anyfs_ts_readlink_p(const char* path, char* buf, size_t cap, int32_t* out)
{
	*out = anyfs_ts_readlink(path, buf, cap);
}

void anyfs_ts_read_kernel_file_p(const char* path, char* buf, size_t cap,
				 int32_t* out)
{
	*out = anyfs_ts_read_kernel_file(path, buf, cap);
}

void anyfs_ts_open_p(const char* path, int flags, int32_t* out)
{
	*out = anyfs_ts_open(path, flags);
}

/* Offset split into low/high 32-bit halves to avoid i64 args (which
 * crash the asyncify rewind path under WASM_BIGINT). */
void anyfs_ts_pread_p(int fd, void* buf, uint32_t n, uint32_t off_lo,
		      uint32_t off_hi, int32_t* out)
{
	int64_t off = (int64_t)(((uint64_t)off_hi << 32) | off_lo);
	int64_t got = anyfs_ts_pread(fd, buf, n, off);
	if (got > 0x7fffffff)
		got = 0x7fffffff;
	*out = (int32_t)got;
}

void anyfs_ts_close_p(int fd, int32_t* out)
{
	*out = anyfs_ts_close(fd);
}

/* PROXY_TO_PTHREAD requires a main(). It runs on the worker pthread that
 * subsequently hosts the LKL kernel CPU thread.
 * emscripten_exit_with_live_runtime yields the pthread back to the event loop
 * without tearing the runtime down, so JS->C ccalls keep working (they're
 * auto-proxied to this same pthread). */
int main(void)
{
#ifdef __EMSCRIPTEN__
	emscripten_exit_with_live_runtime();
#endif
	return 0;
}
