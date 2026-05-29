/*
 * anyfs-fuse — FUSE frontend for anyfs-reader using QEMU block backend
 *
 * Mounts disk images (raw, qcow2, vmdk, etc.) via FUSE.
 * Uses anyfs_disk_* session API for lazy per-partition mounting.
 * FUSE callbacks adapted from Linux tools/lkl/lklfuse.c.
 *
 * Usage: anyfs-fuse [options] <image> [<image2> ...] <mountpoint>
 *   -o fstype=TYPE        filesystem type hint (default: auto-detect)
 *   -o part=<path>        path DSL: p1, p2/p1, disk0/p1 (or bare int
 * back-compat) When given, that partition is mounted at the FUSE root (Mode A).
 * Omit for synthetic partition listing (Mode B). -o prefetch pre-enter every
 * partition at startup (Mode B only) -o ro                 mount read-only -o
 * mem=N              kernel memory in MB (default: 32) -o loglevel=N kernel log
 * level 0-7 (default: 0) -o opts=OPTS          extra mount options (passed
 * through to anyfs_mount)
 *
 * Operating modes
 * ───────────────
 * Mode A (-o part=<path>): single-partition at FUSE root.
 *   Path is translated with LPATH() after a one-time anyfs_disk_enter at
 *   startup — identical behaviour to the old anyfs_mount code, just using
 *   the new session API.
 *
 * Mode B (default): synthetic root listing all partitions.
 *   getattr on "/" or "/pN" or "/diskN" or "/diskN/pM" → synthesised S_IFDIR,
 *   no mount.  opendir/readdir below partition → anyfs_disk_enter fires.
 *   .partitions / partitions.txt are synthetic read-only files at the root
 *   (and at /diskN/ in multi-disk mode).
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 35

#include <errno.h>
#include <fuse3/fuse.h>
#include <fuse3/fuse_opt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Platform type compatibility: Linux libfuse3 uses POSIX types (struct stat,
 * off_t, etc.). WinFSP on native Windows uses its own types (struct fuse_stat,
 * fuse_off_t, etc.). On Cygwin these map back to POSIX.
 *
 * Define portable aliases so callback signatures work on both platforms.
 */
#ifdef _WIN32
/*
 * WinFSP defines these as structs (not typedefs), so we need explicit
 * typedefs to use them without the struct keyword.
 *
 * WinFSP's fuse3/fuse_common.h and fuse/fuse_common.h use the same
 * header guard (FUSE_COMMON_H_), so fuse_parse_cmdline from the latter
 * is blocked. We declare it manually.
 */
#include <fcntl.h>
/* WinFSP doesn't provide POSIX file-mode bits — pull them from
 * mingw's <sys/stat.h>, which defines S_IFDIR/S_IFREG/etc. */
#include <sys/stat.h>
typedef struct fuse_stat fuse_stat;
typedef struct fuse_statvfs fuse_statvfs;
typedef struct fuse_timespec fuse_timespec;

/* O_* flags that exist on Linux but not in MinGW <fcntl.h>.
 * WinFSP never sets these in fi->flags, so defining them to 0 is safe. */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_DSYNC
#define O_DSYNC 0
#endif
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NOATIME
#define O_NOATIME 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif

/* fuse_parse_cmdline: not exported by the WinFSP DLL we have.
 * Implement locally — on Windows we only need mountpoint, foreground,
 * and single-threaded flags. -o options were already handled by
 * fuse_opt_parse above. */
static int fuse_parse_cmdline(struct fuse_args* args, char** mountpoint,
			      int* multithreaded, int* foreground)
{
	int i;
	*mountpoint = NULL;
	*multithreaded = 0;
	*foreground = 0;

	for (i = 0; i < args->argc; i++) {
		if (!args->argv[i])
			continue;
		if (strcmp(args->argv[i], "-f") == 0) {
			*foreground = 1;
			args->argv[i] = NULL;
		} else if (strcmp(args->argv[i], "-s") == 0) {
			*multithreaded = 0;
			args->argv[i] = NULL;
		} else if (strcmp(args->argv[i], "-d") == 0) {
			/* debug — ignore, fuse_new handles it */
		} else if (args->argv[i][0] != '-') {
			if (!*mountpoint)
				*mountpoint = strdup(args->argv[i]);
			args->argv[i] = NULL;
		}
	}
	return 0;
}
#else
// Linux: map portable names to POSIX types
#include <fuse3/fuse_lowlevel.h> // provides fuse_parse_cmdline, fuse_cmdline_opts
#include <sys/stat.h>
typedef struct stat fuse_stat;
typedef struct statvfs fuse_statvfs;
#define fuse_off_t off_t
#define fuse_mode_t mode_t
#define fuse_dev_t dev_t
typedef unsigned int fuse_uid_t;
typedef unsigned int fuse_gid_t;
typedef struct timespec fuse_timespec;
#endif

#include "../src/core/anyfs_disk_dump.h"
#include "../src/core/path_dsl.h"
#include "../src/core/share_spec.h"
#include "anyfs.h"
#include "anyfs_disk.h"

/* ── Multi-disk session state ────────────────────────────────────── */

#define MAX_DISKS 16

/*
 * Per-partition cached LKL path (set after first anyfs_disk_enter).
 * Indexed by 1-based partition number; slot 0 unused.
 * We cap at 64 partitions per disk for static allocation.
 *
 * v2: top-level cache only. Nested chains (e.g. /p2/p1) are mounted
 * via anyfs_disk_walk on each lookup; the disk session de-duplicates
 * via the MOUNTED state, so this is cheap. lkl_path[] caches the
 * resulting LKL mount path so we don't query the slot tree per call.
 */
#define MAX_PARTS_PER_DISK 64

typedef struct {
	char lkl_path[64]; /* filled once MOUNTED; empty string = not yet
			      entered */
} PartCache;

typedef struct {
	AnyfsDisk* disk;
	PartCache
	    parts[MAX_PARTS_PER_DISK]; /* parts[i] → partition i+1 (1-based) */
} DiskSlot;

/* Path-resolver context: parsed comp chain + inner path + result of
 * walking it against the disk session (set by resolve_fuse_path). */
typedef struct {
	int disk_idx;  /* -1 if path is "/" or ".partitions" only */
	AnyfsPath dsl; /* must be released with anyfs_path_dsl_free */
	int dsl_inited;
	char inner[4096]; /* "" if no inner path, else "/sub/...". */
	/* Filled after resolve_walk_chain: */
	int leaf_slot;	    /* -1 if not resolved */
	int leaf_is_fs;	    /* 1 = FS, 0 = container */
	char lkl_mount[64]; /* mount path if FS leaf was mounted */
} FusePath;

static void fuse_path_release(FusePath* fp)
{
	if (fp && fp->dsl_inited) {
		anyfs_path_dsl_free(&fp->dsl);
		fp->dsl_inited = 0;
	}
}

static DiskSlot g_disks[MAX_DISKS];
static int g_ndisks = 0;

/*
 * Mode A: single partition pinned at FUSE root.
 * g_mode_a == 1  → use g_mount_point as simple prefix (fast path).
 * g_mode_a == 0  → Mode B synthetic root.
 */
static int g_mode_a = 0;
static char
    g_mount_point[64]; /* Mode A only — LKL path e.g. /lklmnt/anyfs_d0_p1 */

/* ── Configuration ───────────────────────────────────────────────── */

struct anyfs_fuse_config {
	/* multi-image: we collect them in anyfs_fuse_opt_proc; this field
	 * is kept only for the first positional image for legacy compat */
	char* image; /* first positional image (back-compat; also first of
			images[]) */
	char* part;  /* path DSL string or bare integer */
	int prefetch;
	int readonly;
	int mem_mb;
	int loglevel;
	char* fstype;
	char* opts;
};

/* Additional images beyond the first: collected in opt_proc */
static char* g_extra_images[MAX_DISKS];
static int g_nextra = 0;

static struct anyfs_fuse_config cfg = {
    .part = NULL,
    .prefetch = 0,
    .readonly = 1,
    .mem_mb = 32,
    .loglevel = 0,
};

#define ANYFS_FUSE_OPT(t, p, v) {t, offsetof(struct anyfs_fuse_config, p), v}

enum {
	KEY_HELP,
	KEY_VERSION,
};

static struct fuse_opt anyfs_fuse_opts[] = {
    ANYFS_FUSE_OPT("fstype=%s", fstype, 0),
    ANYFS_FUSE_OPT("part=%s", part, 0),
    ANYFS_FUSE_OPT("prefetch", prefetch, 1),
    ANYFS_FUSE_OPT("mem=%d", mem_mb, 0),
    ANYFS_FUSE_OPT("loglevel=%d", loglevel, 0),
    ANYFS_FUSE_OPT("opts=%s", opts, 0),
    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("--help", KEY_HELP),
    FUSE_OPT_KEY("-V", KEY_VERSION),
    FUSE_OPT_KEY("--version", KEY_VERSION),
    FUSE_OPT_END};

static void usage(void)
{
	printf(
	    "usage: anyfs-fuse [options] <image> [<image2> ...] <mountpoint>\n"
	    "\n"
	    "general options:\n"
	    "    -o opt,[opt...]       mount options\n"
	    "    -h   --help           print help\n"
	    "    -V   --version        print version\n"
	    "\n"
	    "anyfs-fuse options:\n"
	    ""
	    "    -o fstype=TYPE        filesystem type hint (default: "
	    "auto-detect)\n"
	    "    -o part=<path>        path DSL or bare int; pin partition at "
	    "FUSE root\n"
	    "    -o prefetch           pre-enter all partitions at startup\n"
	    "    -o ro                 mount read-only\n"
	    "    -o mem=N              kernel memory in MB (default: 32)\n"
	    "    -o loglevel=N         kernel log level 0-7 (default: 0)\n"
	    "    -o opts=OPTS          extra mount options\n");
}

/*
 * Collect positional non-option arguments:
 *   first  → first disk image (cfg.image)
 *   second, third, …  → additional disk images  (g_extra_images[])
 * The LAST positional arg is the FUSE mountpoint — handled by
 * fuse_parse_cmdline after we're done, so all positionals that reach
 * this callback get stored as potential images.  We reconcile in main()
 * by dropping the last one (the mountpoint).
 */
static int anyfs_fuse_opt_proc(void* data, const char* arg, int key,
			       struct fuse_args* args)
{
	(void)data;

	switch (key) {
	case FUSE_OPT_KEY_OPT:
		if (strcmp(arg, "ro") == 0) {
			cfg.readonly = 1;
			return 1;
		}
		return 1;

	case FUSE_OPT_KEY_NONOPT:
		if (!cfg.image) {
			cfg.image = strdup(arg);
			return 0;
		}
		/* Store in extra list; last one will be the mountpoint — drop
		 * it in main() after fuse_parse_cmdline tells us the
		 * mountpoint. */
		if (g_nextra < MAX_DISKS - 1) {
			g_extra_images[g_nextra++] = strdup(arg);
		}
		return 1; /* keep arg in argv so fuse_parse_cmdline can pick the
			   * last positional as the mountpoint */

	case KEY_HELP:
		usage();
		args->argv[0] = "";
		fuse_opt_add_arg(args, "-h");
		fuse_main(args->argc, args->argv, NULL, NULL);
		exit(1);

	case KEY_VERSION:
		printf("anyfs-fuse version 0.3\n");
		fuse_opt_add_arg(args, "--version");
		fuse_main(args->argc, args->argv, NULL, NULL);
		exit(0);

	default:
		fprintf(stderr, "internal error\n");
		abort();
	}
}

/* ── Path-translation helpers ─────────────────────────────────────── */

/*
 * Mode A fast path: just prepend the single LKL mount point.
 * (Identical to the old lkl_path / LPATH approach.)
 */
static void lkl_path(char* buf, size_t size, const char* path)
{
	if (path[0] == '/')
		path++;
	int n = snprintf(buf, size, "%s/%s", g_mount_point, path);
	if (n < 0 || (size_t)n >= size)
		buf[size - 1] = '\0';
}

#define LPATH_BUF_SIZE 4096
#define LPATH_DECL char _lpath[LPATH_BUF_SIZE]
#define LPATH(path) (lkl_path(_lpath, sizeof(_lpath), path), _lpath)

/*
 * Synthetic file names recognised in Mode B.
 */
#define SYNTH_DOT_PARTITIONS ".partitions"
#define SYNTH_PARTITIONS_TXT "partitions.txt"

/*
 * Mode B path parser.
 *
 * Parses a FUSE path (relative to FUSE root, starting with '/') into its
 * structural components without triggering any LKL mounts.
 *
 * Sets *disk_idx, *part_num, and copies the remainder of the path (the
 * part *below* the partition node) into inner_path.
 *
 * Returns:
 *   0  — *disk_idx / *part_num / inner_path are valid; this is a real path
 *         inside a partition.
 *   1  — path is at the synthetic root ("/" or "/diskN" or "/" only)
 *   2  — path is at a partition node ("/pN" or "/diskN/pM") — no inner path
 *   3  — path is a synthetic file (.partitions / partitions.txt) at root
 *         or at /diskN level; *disk_idx is -1 for root level
 *  -1  — parse error / no such component
 */
typedef enum {
	PATH_KIND_IN_PARTITION = 0,
	PATH_KIND_SYNTH_ROOT = 1,
	PATH_KIND_PART_NODE = 2,
	PATH_KIND_SYNTH_FILE = 3,
} PathKind;

/*
 * Parse a FUSE path into its structural pieces without touching the LKL
 * session. The output FusePath must be released with fuse_path_release.
 *
 * Form:  /[diskN/]<file|pN[?Q](/pM[?Q])*[/inner]>
 *
 * The pN chain is parsed greedily: every segment that matches `pN` (or
 * `pN?query`) becomes a chain component. The first non-pN segment, and
 * everything after it, becomes the inner path. (For container-only
 * chains, inner stays empty.)
 */
static PathKind parse_fuse_path(const char* fuse_path, FusePath* out)
{
	memset(out, 0, sizeof(*out));
	out->disk_idx = -1;
	out->leaf_slot = -1;

	const char* p = fuse_path;
	if (*p == '/')
		p++;

	/* root */
	if (*p == '\0')
		return PATH_KIND_SYNTH_ROOT;

	/* root-level synthetic files */
	if (strcmp(p, SYNTH_DOT_PARTITIONS) == 0 ||
	    strcmp(p, SYNTH_PARTITIONS_TXT) == 0)
		return PATH_KIND_SYNTH_FILE;

	/* Optional /diskN prefix */
	if (strncmp(p, "disk", 4) == 0 && p[4] >= '0' && p[4] <= '9') {
		char* end;
		long v = strtol(p + 4, &end, 10);
		if (*end == '\0') {
			if (v < 0 || v >= g_ndisks)
				return -1;
			out->disk_idx = (int)v;
			return PATH_KIND_SYNTH_ROOT;
		}
		if (*end == '/') {
			if (v < 0 || v >= g_ndisks)
				return -1;
			out->disk_idx = (int)v;
			p = end + 1;
			if (strcmp(p, SYNTH_DOT_PARTITIONS) == 0 ||
			    strcmp(p, SYNTH_PARTITIONS_TXT) == 0)
				return PATH_KIND_SYNTH_FILE;
		}
	} else if (g_ndisks > 1) {
		return -1;
	}

	/* Must start with 'p' to be a partition chain. */
	if (*p != 'p')
		return -1;
	if (out->disk_idx < 0)
		out->disk_idx = 0;

	/* Find the first non-pN segment ("/foo" that isn't /pN). */
	const char* chain_end = p;
	while (chain_end && *chain_end) {
		const char* seg = chain_end;
		const char* slash = strchr(seg, '/');
		const char* seg_end = slash ? slash : seg + strlen(seg);

		/* Validate: must be `p<digits>` optionally followed by `?...`
		 */
		if (seg[0] != 'p')
			break;
		const char* q = seg + 1;
		if (q >= seg_end || *q < '0' || *q > '9')
			break;
		while (q < seg_end && *q >= '0' && *q <= '9')
			q++;
		if (q < seg_end && *q != '?')
			break;

		chain_end = slash ? slash + 1 : NULL;
	}
	/* chain_end now points at either '\0' (chain is the whole path) or
	 * the first non-pN segment, which becomes inner_path. */

	/* Build a DSL string covering only the p-chain. */
	char dsl_buf[512];
	size_t chain_len;
	if (chain_end == NULL)
		chain_len = strlen(p);
	else
		chain_len =
		    (size_t)((chain_end - 1) - p); /* exclude trailing '/' */
	if (chain_len == 0 || chain_len >= sizeof(dsl_buf))
		return -1;
	memcpy(dsl_buf, p, chain_len);
	dsl_buf[chain_len] = '\0';

	if (anyfs_path_dsl_parse(dsl_buf, &out->dsl) < 0)
		return -1;
	out->dsl_inited = 1;

	/* Remaining inner path (with leading '/'). */
	if (chain_end && *chain_end) {
		out->inner[0] = '/';
		snprintf(out->inner + 1, sizeof(out->inner) - 1, "%s",
			 chain_end);
		return PATH_KIND_IN_PARTITION;
	}
	return PATH_KIND_PART_NODE;
}

/*
 * Resolve a FusePath against the disk session: walk the comp chain
 * (entering containers along the way) and, when the leaf is FS, mount
 * it. On success fp->leaf_slot, fp->leaf_is_fs, fp->lkl_mount are set.
 * Returns 0 on success, negative errno-style on failure.
 *
 * Caches the top-level partition's mount path in g_disks[].parts[]
 * so back-compat lookups stay fast.
 */
static int resolve_walk(FusePath* fp)
{
	if (fp->disk_idx < 0 || fp->disk_idx >= g_ndisks)
		return -ENOENT;
	if (!fp->dsl_inited)
		return -ENOENT;
	AnyfsDisk* d = g_disks[fp->disk_idx].disk;
	int leaf = -1;
	char lkl_path[64] = {0};
	int rc = anyfs_disk_walk(d, fp->dsl.comp, fp->dsl.n_comp, 0, &leaf,
				 lkl_path);
	if (rc < 0)
		return -EIO;
	fp->leaf_slot = leaf;
	fp->leaf_is_fs = (lkl_path[0] != '\0') ? 1 : 0;
	snprintf(fp->lkl_mount, sizeof(fp->lkl_mount), "%s", lkl_path);

	/* Back-compat: cache the LKL mount path for the top-level part in
	 * the legacy PartCache so anything still calling the old path
	 * (e.g. -o prefetch) keeps working. */
	if (fp->leaf_is_fs && fp->dsl.n_comp == 1) {
		unsigned int pn = fp->dsl.comp[0].p;
		if (pn > 0 && pn <= MAX_PARTS_PER_DISK)
			snprintf(
			    g_disks[fp->disk_idx].parts[pn - 1].lkl_path,
			    sizeof(
				g_disks[fp->disk_idx].parts[pn - 1].lkl_path),
			    "%s", lkl_path);
	}
	return 0;
}

/*
 * Mode B full path translation: fuse_path → LKL absolute path in out.
 * Used only for IN_PARTITION; PART_NODE / synthetic kinds handle their
 * own dispatch in the caller. Returns 0 on success, negative errno-style.
 */
static int fuse_path_to_lkl(const char* fuse_path, char* out, size_t out_len)
{
	FusePath fp;
	PathKind kind = parse_fuse_path(fuse_path, &fp);
	int rc = -ENOENT;
	if (kind == PATH_KIND_IN_PARTITION) {
		if (resolve_walk(&fp) == 0 && fp.leaf_is_fs) {
			snprintf(out, out_len, "%s%s", fp.lkl_mount, fp.inner);
			rc = 0;
		} else {
			rc = -EIO;
		}
	}
	fuse_path_release(&fp);
	return rc;
}

/* ── Synthetic content helpers ────────────────────────────────────── */

/*
 * Build the .partitions / partitions.txt table into a heap buffer.
 * Caller must free() the returned pointer.
 * *len is set to the content length (without NUL).
 * disk_idx == -1  → unified table across all disks (root level).
 * disk_idx >= 0   → table for that disk only.
 */
static char* build_partitions_table(int disk_idx, size_t* len)
{
	AnyfsStrbuf sb;
	anyfs_strbuf_init(&sb);
	anyfs_dump_header(&sb);
	if (disk_idx < 0) {
		for (int i = 0; i < g_ndisks; i++)
			anyfs_dump_disk(&sb, g_disks[i].disk, i);
	} else {
		if (disk_idx < g_ndisks)
			anyfs_dump_disk(&sb, g_disks[disk_idx].disk, disk_idx);
	}
	return anyfs_strbuf_detach(&sb, len);
}

/* ── FUSE operations (adapted from lklfuse.c) ─────────────────────── */

static void xlat_stat(const struct lkl_stat* in, fuse_stat* st)
{
	memset(st, 0, sizeof(*st));
	st->st_dev = in->st_dev;
	st->st_ino = in->st_ino;
	st->st_mode = in->st_mode;
	st->st_nlink = in->st_nlink;
	st->st_uid = in->st_uid;
	st->st_gid = in->st_gid;
	st->st_rdev = in->st_rdev;
	st->st_size = in->st_size;
	st->st_blksize = in->st_blksize;
	st->st_blocks = in->st_blocks;
	st->st_atim.tv_sec = in->lkl_st_atime;
	st->st_atim.tv_nsec = in->st_atime_nsec;
	st->st_mtim.tv_sec = in->lkl_st_mtime;
	st->st_mtim.tv_nsec = in->st_mtime_nsec;
	st->st_ctim.tv_sec = in->lkl_st_ctime;
	st->st_ctim.tv_nsec = in->st_ctime_nsec;
}

/* Synthesise a directory stat (used for partition nodes in Mode B). */
static void synth_dir_stat(fuse_stat* st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFDIR | 0555;
	st->st_nlink = 2;
}

/* Synthesise a regular-file stat for synthetic text files. */
static void synth_file_stat(fuse_stat* st, size_t sz)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0444;
	st->st_nlink = 1;
	st->st_size = (fuse_off_t)sz;
}

static int anyfs_fuse_getattr(const char* path, fuse_stat* st,
			      struct fuse_file_info* fi)
{
	/* Mode A: straight passthrough */
	if (g_mode_a) {
		LPATH_DECL;
		struct lkl_stat lkl_stat;
		long ret;
		if (fi)
			ret = lkl_sys_fstat(fi->fh, &lkl_stat);
		else
			ret = lkl_sys_lstat(LPATH(path), &lkl_stat);
		if (!ret)
			xlat_stat(&lkl_stat, st);
		return ret;
	}

	/* Mode B */
	FusePath fp;
	PathKind kind = parse_fuse_path(path, &fp);
	int rc;
	switch (kind) {
	case PATH_KIND_SYNTH_ROOT:
		synth_dir_stat(st);
		rc = 0;
		break;

	case PATH_KIND_PART_NODE: {
		/* Walk against the slot tree (this also materialises any
		 * container along the chain, which is exactly what we want so
		 * that a subsequent getattr on /pN/pM has children listed). */
		if (resolve_walk(&fp) < 0) {
			rc = -ENOENT;
			break;
		}
		if (fp.leaf_is_fs) {
			struct lkl_stat lkl_stat;
			long ret = lkl_sys_lstat(fp.lkl_mount, &lkl_stat);
			if (!ret)
				xlat_stat(&lkl_stat, st);
			rc = (int)ret;
		} else {
			synth_dir_stat(st);
			rc = 0;
		}
		break;
	}

	case PATH_KIND_SYNTH_FILE: {
		size_t len = 0;
		char* content = build_partitions_table(fp.disk_idx, &len);
		if (!content) {
			rc = -EIO;
			break;
		}
		free(content);
		synth_file_stat(st, len);
		rc = 0;
		break;
	}

	case PATH_KIND_IN_PARTITION: {
		if (resolve_walk(&fp) < 0 || !fp.leaf_is_fs) {
			rc = -ENOENT;
			break;
		}
		char lkl_full[LPATH_BUF_SIZE];
		snprintf(lkl_full, sizeof(lkl_full), "%s%s", fp.lkl_mount,
			 fp.inner);
		struct lkl_stat lkl_stat;
		long ret;
		if (fi)
			ret = lkl_sys_fstat(fi->fh, &lkl_stat);
		else
			ret = lkl_sys_lstat(lkl_full, &lkl_stat);
		if (!ret)
			xlat_stat(&lkl_stat, st);
		rc = (int)ret;
		break;
	}

	default:
		rc = -ENOENT;
		break;
	}
	fuse_path_release(&fp);
	return rc;
}

static int anyfs_fuse_readlink(const char* path, char* buf, size_t len)
{
	if (g_mode_a) {
		LPATH_DECL;
		long ret = lkl_sys_readlink(LPATH(path), buf, len);
		if (ret < 0)
			return ret;
		if ((size_t)ret == len)
			ret = len - 1;
		buf[ret] = 0;
		return 0;
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	long ret = lkl_sys_readlink(lkl_full, buf, len);
	if (ret < 0)
		return ret;
	if ((size_t)ret == len)
		ret = len - 1;
	buf[ret] = 0;
	return 0;
}

static int anyfs_fuse_mknod(const char* path, fuse_mode_t mode, fuse_dev_t dev)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_mknod(LPATH(path), mode, dev);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_mknod(lkl_full, mode, dev);
}

static int anyfs_fuse_mkdir(const char* path, fuse_mode_t mode)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_mkdir(LPATH(path), mode);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_mkdir(lkl_full, mode);
}

static int anyfs_fuse_unlink(const char* path)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_unlink(LPATH(path));
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_unlink(lkl_full);
}

static int anyfs_fuse_rmdir(const char* path)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_rmdir(LPATH(path));
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_rmdir(lkl_full);
}

static int anyfs_fuse_symlink(const char* oldname, const char* newname)
{
	/* oldname = symlink target (string, not a path — skip path translation)
	 */
	if (g_mode_a) {
		char _lpath2[LPATH_BUF_SIZE];
		lkl_path(_lpath2, sizeof(_lpath2), newname);
		return lkl_sys_symlink(oldname, _lpath2);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(newname, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_symlink(oldname, lkl_full);
}

static int anyfs_fuse_rename(const char* oldname, const char* newname,
			     unsigned int flags)
{
	if (g_mode_a) {
		LPATH_DECL;
		char _lpath2[LPATH_BUF_SIZE];
		lkl_path(_lpath2, sizeof(_lpath2), newname);
		return lkl_sys_renameat2(LKL_AT_FDCWD, LPATH(oldname),
					 LKL_AT_FDCWD, _lpath2, flags);
	}
	char old_full[LPATH_BUF_SIZE], new_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(oldname, old_full, sizeof(old_full));
	if (r < 0)
		return r;
	r = fuse_path_to_lkl(newname, new_full, sizeof(new_full));
	if (r < 0)
		return r;
	return lkl_sys_renameat2(LKL_AT_FDCWD, old_full, LKL_AT_FDCWD, new_full,
				 flags);
}

static int anyfs_fuse_link(const char* oldname, const char* newname)
{
	if (g_mode_a) {
		LPATH_DECL;
		char _lpath2[LPATH_BUF_SIZE];
		lkl_path(_lpath2, sizeof(_lpath2), newname);
		return lkl_sys_link(LPATH(oldname), _lpath2);
	}
	char old_full[LPATH_BUF_SIZE], new_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(oldname, old_full, sizeof(old_full));
	if (r < 0)
		return r;
	r = fuse_path_to_lkl(newname, new_full, sizeof(new_full));
	if (r < 0)
		return r;
	return lkl_sys_link(old_full, new_full);
}

static int anyfs_fuse_chmod(const char* path, fuse_mode_t mode,
			    struct fuse_file_info* fi)
{
	if (g_mode_a) {
		LPATH_DECL;
		if (fi)
			return lkl_sys_fchmod(fi->fh, mode);
		return lkl_sys_fchmodat(LKL_AT_FDCWD, LPATH(path), mode);
	}
	if (fi)
		return lkl_sys_fchmod(fi->fh, mode);
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_fchmodat(LKL_AT_FDCWD, lkl_full, mode);
}

static int anyfs_fuse_chown(const char* path, fuse_uid_t uid, fuse_gid_t gid,
			    struct fuse_file_info* fi)
{
	if (g_mode_a) {
		LPATH_DECL;
		if (fi)
			return lkl_sys_fchown(fi->fh, uid, gid);
		return lkl_sys_fchownat(LKL_AT_FDCWD, LPATH(path), uid, gid,
					LKL_AT_SYMLINK_NOFOLLOW);
	}
	if (fi)
		return lkl_sys_fchown(fi->fh, uid, gid);
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_fchownat(LKL_AT_FDCWD, lkl_full, uid, gid,
				LKL_AT_SYMLINK_NOFOLLOW);
}

static int anyfs_fuse_truncate(const char* path, fuse_off_t off,
			       struct fuse_file_info* fi)
{
	if (g_mode_a) {
		LPATH_DECL;
		if (fi)
			return lkl_sys_ftruncate(fi->fh, off);
		return lkl_sys_truncate(LPATH(path), off);
	}
	if (fi)
		return lkl_sys_ftruncate(fi->fh, off);
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_truncate(lkl_full, off);
}

/*
 * Shared open3 helper: build LKL open flags and call lkl_sys_open.
 * `lkl_path_str` must already be the resolved absolute LKL path.
 */
static int open3_lkl(const char* lkl_path_str, bool create, fuse_mode_t mode,
		     struct fuse_file_info* fi)
{
	long ret;
	int flags = 0;

	switch (fi->flags & O_ACCMODE) {
	case O_RDONLY:
		flags = LKL_O_RDONLY;
		break;
	case O_WRONLY:
		flags = LKL_O_WRONLY;
		break;
	case O_RDWR:
		flags = LKL_O_RDWR;
		break;
	default:
		return -EINVAL;
	}

	if (create)
		flags |= LKL_O_CREAT;
	if (fi->flags & O_TRUNC)
		flags |= LKL_O_TRUNC;
	if (fi->flags & O_APPEND)
		flags |= LKL_O_APPEND;
	if (fi->flags & O_NONBLOCK)
		flags |= LKL_O_NONBLOCK;
	if (fi->flags & O_DSYNC)
		flags |= LKL_O_DSYNC;
	if (fi->flags & O_DIRECT)
		flags |= LKL_O_DIRECT;
	if (fi->flags & O_LARGEFILE)
		flags |= LKL_O_LARGEFILE;
	if (fi->flags & O_DIRECTORY)
		flags |= LKL_O_DIRECTORY;
	if (fi->flags & O_NOFOLLOW)
		flags |= LKL_O_NOFOLLOW;
	if (fi->flags & O_NOATIME)
		flags |= LKL_O_NOATIME;
	if (fi->flags & O_CLOEXEC)
		flags |= LKL_O_CLOEXEC;
	if (fi->flags & O_SYNC)
		flags |= LKL_O_SYNC;

	ret = lkl_sys_open(lkl_path_str, flags, mode);
	if (ret < 0)
		return ret;
	fi->fh = ret;
	return 0;
}

static int anyfs_fuse_create(const char* path, fuse_mode_t mode,
			     struct fuse_file_info* fi)
{
	if (g_mode_a) {
		LPATH_DECL;
		return open3_lkl(LPATH(path), true, mode, fi);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return open3_lkl(lkl_full, true, mode, fi);
}

static int anyfs_fuse_open(const char* path, struct fuse_file_info* fi)
{
	if (g_mode_a) {
		LPATH_DECL;
		return open3_lkl(LPATH(path), false, 0, fi);
	}

	/* Mode B: synthetic files (.partitions / partitions.txt) have no LKL
	 * backing. Mark with fi->fh = 0 so read/release short-circuit. */
	FusePath fp;
	PathKind kind = parse_fuse_path(path, &fp);
	fuse_path_release(&fp);
	if (kind == PATH_KIND_SYNTH_FILE) {
		fi->fh = 0;
		return 0;
	}

	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return open3_lkl(lkl_full, false, 0, fi);
}

static int anyfs_fuse_read(const char* path, char* buf, size_t size,
			   fuse_off_t offset, struct fuse_file_info* fi)
{
	(void)path;

	/* Mode B synthetic file reads are handled here — fi->fh is not set for
	 * synthetic files because we don't call open for them; instead we
	 * detect by checking fi->fh == 0 and path being a synth file.
	 * TODO(v2): use fuse direct_io + per-file context for synthetic files.
	 * For now, rebuild the buffer on every read (content is tiny). */
	if (!g_mode_a) {
		FusePath fp;
		PathKind kind = parse_fuse_path(path, &fp);
		int rdisk = fp.disk_idx;
		fuse_path_release(&fp);
		if (kind == PATH_KIND_SYNTH_FILE) {
			size_t content_len = 0;
			char* content =
			    build_partitions_table(rdisk, &content_len);
			if (!content)
				return -EIO;
			ssize_t ret = 0;
			if ((size_t)offset < content_len) {
				size_t avail = content_len - (size_t)offset;
				size_t n = avail < size ? avail : size;
				memcpy(buf, content + offset, n);
				ret = (ssize_t)n;
			}
			free(content);
			return (int)ret;
		}
	}

	long ret;
	ssize_t orig_size = size;
	do {
		ret = lkl_sys_pread64(fi->fh, buf, size, offset);
		if (ret <= 0)
			break;
		size -= ret;
		offset += ret;
		buf += ret;
	} while (size > 0);
	return ret < 0 ? ret : orig_size - (ssize_t)size;
}

static int anyfs_fuse_write(const char* path, const char* buf, size_t size,
			    fuse_off_t offset, struct fuse_file_info* fi)
{
	(void)path;
	long ret;
	ssize_t orig_size = size;
	do {
		ret = lkl_sys_pwrite64(fi->fh, buf, size, offset);
		if (ret <= 0)
			break;
		size -= ret;
		offset += ret;
		buf += ret;
	} while (size > 0);
	return ret < 0 ? ret : orig_size - (ssize_t)size;
}

static int anyfs_fuse_statfs(const char* path, fuse_statvfs* stat)
{
	const char* target;
	char lkl_full[LPATH_BUF_SIZE];
	if (g_mode_a) {
		LPATH_DECL;
		target = LPATH(path);
		/* LPATH writes into _lpath on the stack; copy so we have a
		 * stable ptr */
		snprintf(lkl_full, sizeof(lkl_full), "%s", target);
		target = lkl_full;
	} else {
		int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
		if (r < 0)
			return r;
		target = lkl_full;
	}

	struct lkl_statfs lkl_statfs;
	long ret = lkl_sys_statfs(target, &lkl_statfs);
	if (ret < 0)
		return ret;
	memset(stat, 0, sizeof(*stat));
	stat->f_bsize = lkl_statfs.f_bsize;
	stat->f_frsize = lkl_statfs.f_frsize;
	stat->f_blocks = lkl_statfs.f_blocks;
	stat->f_bfree = lkl_statfs.f_bfree;
	stat->f_bavail = lkl_statfs.f_bavail;
	stat->f_files = lkl_statfs.f_files;
	stat->f_ffree = lkl_statfs.f_ffree;
	stat->f_favail = lkl_statfs.f_ffree;
	stat->f_fsid = *(unsigned long*)&lkl_statfs.f_fsid.val[0];
	stat->f_flag = lkl_statfs.f_flags;
	stat->f_namemax = lkl_statfs.f_namelen;
	return 0;
}

static int anyfs_fuse_flush(const char* path, struct fuse_file_info* fi)
{
	(void)path;
	(void)fi;
	return 0;
}

static int anyfs_fuse_release(const char* path, struct fuse_file_info* fi)
{
	(void)path;
	if (fi->fh == 0)
		return 0; /* synthetic file — nothing to close */
	return lkl_sys_close(fi->fh);
}

static int anyfs_fuse_fsync(const char* path, int datasync,
			    struct fuse_file_info* fi)
{
	(void)path;
	if (datasync)
		return lkl_sys_fdatasync(fi->fh);
	return lkl_sys_fsync(fi->fh);
}

static int anyfs_fuse_setxattr(const char* path, const char* name,
			       const char* val, size_t size, int flags)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_setxattr(LPATH(path), name, val, size, flags);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_setxattr(lkl_full, name, val, size, flags);
}

static int anyfs_fuse_getxattr(const char* path, const char* name, char* val,
			       size_t size)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_getxattr(LPATH(path), name, val, size);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_getxattr(lkl_full, name, val, size);
}

static int anyfs_fuse_listxattr(const char* path, char* list, size_t size)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_listxattr(LPATH(path), list, size);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_listxattr(lkl_full, list, size);
}

static int anyfs_fuse_removexattr(const char* path, const char* name)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_removexattr(LPATH(path), name);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_removexattr(lkl_full, name);
}

/*
 * opendir: for Mode B this is where lazy mount fires for partition dirs.
 * Synthetic root / partition-node dirs are served without an LKL fd.
 * We encode the dir pointer into fi->fh; NULL (0) means synthetic.
 */
static int anyfs_fuse_opendir(const char* path, struct fuse_file_info* fi)
{
	if (g_mode_a) {
		LPATH_DECL;
		int err;
		struct lkl_dir* dir = lkl_opendir(LPATH(path), &err);
		if (!dir)
			return err;
		fi->fh = (uintptr_t)dir;
		return 0;
	}

	/* Mode B */
	FusePath fp;
	PathKind kind = parse_fuse_path(path, &fp);
	int rc;

	switch (kind) {
	case PATH_KIND_SYNTH_ROOT:
		/* Synthetic listing — no LKL open needed */
		fi->fh = 0;
		rc = 0;
		break;

	case PATH_KIND_PART_NODE: {
		/* The partition dir itself: walk fires here. If leaf is FS we
		 * open the mounted root inside LKL. If leaf is a container, we
		 * stay synthetic — readdir will list children. */
		if (resolve_walk(&fp) < 0) {
			rc = -ENOENT;
			break;
		}
		if (fp.leaf_is_fs) {
			int err;
			struct lkl_dir* dir = lkl_opendir(fp.lkl_mount, &err);
			if (!dir) {
				rc = err;
				break;
			}
			fi->fh = (uintptr_t)dir;
		} else {
			fi->fh = 0; /* synth: readdir parses path again */
		}
		rc = 0;
		break;
	}

	case PATH_KIND_IN_PARTITION: {
		if (resolve_walk(&fp) < 0 || !fp.leaf_is_fs) {
			rc = -ENOENT;
			break;
		}
		char lkl_full[LPATH_BUF_SIZE];
		snprintf(lkl_full, sizeof(lkl_full), "%s%s", fp.lkl_mount,
			 fp.inner);
		int err;
		struct lkl_dir* dir = lkl_opendir(lkl_full, &err);
		if (!dir) {
			rc = err;
			break;
		}
		fi->fh = (uintptr_t)dir;
		rc = 0;
		break;
	}

	default:
		rc = -ENOENT;
		break;
	}
	fuse_path_release(&fp);
	return rc;
}

/*
 * readdir: handles both synthetic root listing and real partition dirs.
 *
 * Synthetic root single-disk: p1, p2, …, .partitions, partitions.txt
 * Synthetic root multi-disk:  disk0, disk1, …, .partitions, partitions.txt
 * Synthetic per-disk dir:     p1, p2, …, .partitions, partitions.txt
 */
static int anyfs_fuse_readdir(const char* path, void* buf, fuse_fill_dir_t fill,
			      fuse_off_t off, struct fuse_file_info* fi,
			      enum fuse_readdir_flags flags)
{
	(void)off;
	(void)flags;

	/* Mode A: plain LKL passthrough */
	if (g_mode_a) {
		(void)path;
		struct lkl_dir* dir = (struct lkl_dir*)(uintptr_t)fi->fh;
		struct lkl_linux_dirent64* de;
		while ((de = lkl_readdir(dir))) {
			fuse_stat st;
			memset(&st, 0, sizeof(st));
			st.st_ino = de->d_ino;
			st.st_mode = de->d_type << 12;
			if (fill(buf, de->d_name, &st, 0,
				 (enum fuse_fill_dir_flags)0))
				break;
		}
		if (!de)
			return lkl_errdir(dir);
		return 0;
	}

	/* Mode B */
	FusePath fp;
	PathKind kind = parse_fuse_path(path, &fp);

	if (kind == PATH_KIND_SYNTH_ROOT) {
		fuse_stat st;
		fill(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
		fill(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);

		int didx = fp.disk_idx;
		if (didx < 0 && g_ndisks == 1) {
			/* Single-disk root: list pN entries */
			AnyfsPartInfo pbuf[MAX_PARTS_PER_DISK];
			size_t got = 0;
			anyfs_disk_list(g_disks[0].disk, pbuf,
					MAX_PARTS_PER_DISK, &got);
			char name[16];
			for (size_t i = 0; i < got; i++) {
				snprintf(name, sizeof(name), "p%u",
					 pbuf[i].index);
				synth_dir_stat(&st);
				fill(buf, name, &st, 0,
				     (enum fuse_fill_dir_flags)0);
			}
		} else if (didx < 0) {
			/* Multi-disk root: list diskN entries */
			char name[16];
			for (int i = 0; i < g_ndisks; i++) {
				snprintf(name, sizeof(name), "disk%d", i);
				synth_dir_stat(&st);
				fill(buf, name, &st, 0,
				     (enum fuse_fill_dir_flags)0);
			}
		} else {
			/* Per-disk dir in multi-disk mode */
			AnyfsPartInfo pbuf[MAX_PARTS_PER_DISK];
			size_t got = 0;
			anyfs_disk_list(g_disks[didx].disk, pbuf,
					MAX_PARTS_PER_DISK, &got);
			char name[16];
			for (size_t i = 0; i < got; i++) {
				snprintf(name, sizeof(name), "p%u",
					 pbuf[i].index);
				synth_dir_stat(&st);
				fill(buf, name, &st, 0,
				     (enum fuse_fill_dir_flags)0);
			}
		}

		/* Synthetic metadata files */
		size_t len = 0;
		char* content = build_partitions_table(didx, &len);
		if (content) {
			synth_file_stat(&st, len);
			free(content);
			fill(buf, SYNTH_DOT_PARTITIONS, &st, 0,
			     (enum fuse_fill_dir_flags)0);
			fill(buf, SYNTH_PARTITIONS_TXT, &st, 0,
			     (enum fuse_fill_dir_flags)0);
		}
		fuse_path_release(&fp);
		return 0;
	}

	if (kind == PATH_KIND_PART_NODE) {
		/* Either a container (synth list of children) or an FS root
		 * (real LKL dir). The opendir handler set fi->fh accordingly.
		 */
		if (fi->fh == 0) {
			/* Container: list children via the slot tree.
			 * resolve_walk was already called by opendir; redo it
			 * to find leaf slot (we don't carry context across
			 * calls). */
			fuse_stat st;
			fill(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
			fill(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);
			if (resolve_walk(&fp) == 0 && !fp.leaf_is_fs) {
				AnyfsPartInfo kids[MAX_PARTS_PER_DISK];
				size_t got = 0;
				anyfs_disk_list_children(
				    g_disks[fp.disk_idx].disk, fp.leaf_slot,
				    kids, MAX_PARTS_PER_DISK, &got);
				char name[16];
				for (size_t i = 0; i < got; i++) {
					snprintf(name, sizeof(name), "p%u",
						 kids[i].index);
					synth_dir_stat(&st);
					fill(buf, name, &st, 0,
					     (enum fuse_fill_dir_flags)0);
				}
			}
			fuse_path_release(&fp);
			return 0;
		}
		/* fall through: real LKL dir handler below */
	}
	fuse_path_release(&fp);

	/* Real LKL dir (partition root or deeper) */
	struct lkl_dir* dir = (struct lkl_dir*)(uintptr_t)fi->fh;
	if (!dir)
		return -EIO;
	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir))) {
		fuse_stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (fill(buf, de->d_name, &st, 0, (enum fuse_fill_dir_flags)0))
			break;
	}
	if (!de)
		return lkl_errdir(dir);
	return 0;
}

static int anyfs_fuse_releasedir(const char* path, struct fuse_file_info* fi)
{
	(void)path;
	if (fi->fh == 0)
		return 0; /* synthetic dir */
	return lkl_closedir((struct lkl_dir*)(uintptr_t)fi->fh);
}

static int anyfs_fuse_fsyncdir(const char* path, int datasync,
			       struct fuse_file_info* fi)
{
	(void)path;
	if (fi->fh == 0)
		return 0;
	struct lkl_dir* dir = (struct lkl_dir*)(uintptr_t)fi->fh;
	int fd = lkl_dirfd(dir);
	if (datasync)
		return lkl_sys_fdatasync(fd);
	return lkl_sys_fsync(fd);
}

static int anyfs_fuse_access(const char* path, int mode)
{
	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_access(LPATH(path), mode);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0) {
		/* access on synthetic nodes (root, partition dirs, metadata
		 * files) is allowed */
		FusePath fp;
		PathKind kind = parse_fuse_path(path, &fp);
		fuse_path_release(&fp);
		if (kind == PATH_KIND_SYNTH_ROOT ||
		    kind == PATH_KIND_PART_NODE || kind == PATH_KIND_SYNTH_FILE)
			return 0;
		return r;
	}
	return lkl_sys_access(lkl_full, mode);
}

static int anyfs_fuse_utimens(const char* path, const fuse_timespec tv[2],
			      struct fuse_file_info* fi)
{
	struct __lkl__kernel_timespec ts[2] = {
	    {.tv_sec = tv[0].tv_sec, .tv_nsec = tv[0].tv_nsec},
	    {.tv_sec = tv[1].tv_sec, .tv_nsec = tv[1].tv_nsec},
	};

	if (fi)
		return lkl_sys_utimensat(fi->fh, NULL, ts, 0);

	if (g_mode_a) {
		LPATH_DECL;
		return lkl_sys_utimensat(-1, LPATH(path), ts,
					 LKL_AT_SYMLINK_NOFOLLOW);
	}
	char lkl_full[LPATH_BUF_SIZE];
	int r = fuse_path_to_lkl(path, lkl_full, sizeof(lkl_full));
	if (r < 0)
		return r;
	return lkl_sys_utimensat(-1, lkl_full, ts, LKL_AT_SYMLINK_NOFOLLOW);
}

static int anyfs_fuse_fallocate(const char* path, int mode, fuse_off_t offset,
				fuse_off_t len, struct fuse_file_info* fi)
{
	(void)path;
	return lkl_sys_fallocate(fi->fh, mode, offset, len);
}

static ssize_t
anyfs_fuse_copy_file_range(const char* path_in, struct fuse_file_info* fi_in,
			   fuse_off_t off_in, const char* path_out,
			   struct fuse_file_info* fi_out, fuse_off_t off_out,
			   size_t len, int flags)
{
	(void)path_in;
	(void)path_out;
	lkl_loff_t loff_in = off_in, loff_out = off_out;
	return lkl_sys_copy_file_range(fi_in->fh, &loff_in, fi_out->fh,
				       &loff_out, len, flags);
}

static fuse_off_t anyfs_fuse_lseek(const char* path, fuse_off_t off, int whence,
				   struct fuse_file_info* fi)
{
	(void)path;
	return lkl_sys_lseek(fi->fh, off, whence);
}

static void* anyfs_fuse_init(struct fuse_conn_info* conn,
			     struct fuse_config* cfg2)
{
	(void)conn;
	/* Keep nullpath_ok = 0: many handlers (readdir/getattr/read) dispatch
	 * on the path, and libfuse caching it for us is simpler than threading
	 * a per-handle context struct through every operation. */
	cfg2->nullpath_ok = 0;
	cfg2->entry_timeout = 0;
	cfg2->attr_timeout = 0;
	cfg2->negative_timeout = 0;
	cfg2->use_ino = 1;
	return NULL;
}

static const struct fuse_operations anyfs_fuse_ops = {
    .init = anyfs_fuse_init,
    .getattr = anyfs_fuse_getattr,
    .readlink = anyfs_fuse_readlink,
    .mknod = anyfs_fuse_mknod,
    .mkdir = anyfs_fuse_mkdir,
    .unlink = anyfs_fuse_unlink,
    .rmdir = anyfs_fuse_rmdir,
    .symlink = anyfs_fuse_symlink,
    .rename = anyfs_fuse_rename,
    .link = anyfs_fuse_link,
    .chmod = anyfs_fuse_chmod,
    .chown = anyfs_fuse_chown,
    .truncate = anyfs_fuse_truncate,
    .open = anyfs_fuse_open,
    .read = anyfs_fuse_read,
    .write = anyfs_fuse_write,
    .statfs = anyfs_fuse_statfs,
    .flush = anyfs_fuse_flush,
    .release = anyfs_fuse_release,
    .fsync = anyfs_fuse_fsync,
    .setxattr = anyfs_fuse_setxattr,
    .getxattr = anyfs_fuse_getxattr,
    .listxattr = anyfs_fuse_listxattr,
    .removexattr = anyfs_fuse_removexattr,
    .opendir = anyfs_fuse_opendir,
    .readdir = anyfs_fuse_readdir,
    .releasedir = anyfs_fuse_releasedir,
    .fsyncdir = anyfs_fuse_fsyncdir,
    .access = anyfs_fuse_access,
    .create = anyfs_fuse_create,
    .utimens = anyfs_fuse_utimens,
    .fallocate = anyfs_fuse_fallocate,
#ifndef _WIN32
    .copy_file_range = anyfs_fuse_copy_file_range,
    .lseek = anyfs_fuse_lseek,
#endif
};

/* ── Main ──────────────────────────────────────────────────────────── */

int main(int argc, char* argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse* fuse = NULL;
	char* cli_mountpoint = NULL;
	int cli_foreground = 0;
	int cli_singlethread = 0;
	int ret = 1;
	int kernel_started = 0;

	if (fuse_opt_parse(&args, &cfg, anyfs_fuse_opts, anyfs_fuse_opt_proc))
		return 1;

	if (!cfg.image) {
		fprintf(stderr, "error: no disk image specified\n");
		fprintf(stderr, "usage: anyfs-fuse [options] <image> [<image2> "
				"...] <mountpoint>\n");
		goto out;
	}

	/*
	 * fuse_parse_cmdline signature differs between Linux libfuse3 and
	 * WinFSP.
	 */
#ifdef _WIN32
	{
		int multithreaded = 0;
		if (fuse_parse_cmdline(&args, &cli_mountpoint, &multithreaded,
				       &cli_foreground))
			goto out;
		cli_singlethread = !multithreaded;
	}
#else
	{
		struct fuse_cmdline_opts cli_opts;
		memset(&cli_opts, 0, sizeof(cli_opts));
		if (fuse_parse_cmdline(&args, &cli_opts))
			goto out;
		cli_mountpoint = cli_opts.mountpoint;
		cli_foreground = cli_opts.foreground;
		cli_singlethread = cli_opts.singlethread;
	}
#endif

	if (!cli_mountpoint) {
		fprintf(stderr, "error: no mount point specified\n");
		goto out;
	}

	/*
	 * The opt_proc stored all positional non-option args as potential
	 * images (including the mountpoint).  fuse_parse_cmdline now tells us
	 * the actual mountpoint, so drop it from g_extra_images if it's the
	 * last one we stored.
	 */
	if (g_nextra > 0 &&
	    strcmp(g_extra_images[g_nextra - 1], cli_mountpoint) == 0) {
		free(g_extra_images[g_nextra - 1]);
		g_extra_images[--g_nextra] = NULL;
	}

	/*
	 * Back-compat: bare integer in -o part=N → "pN".
	 * E.g. "-o part=2" → "p2".
	 */
	char part_dsl_buf[32];
	char* part_str = cfg.part;
	if (part_str) {
		char* endp;
		long pv = strtol(part_str, &endp, 10);
		if (*endp == '\0' && pv > 0) {
			snprintf(part_dsl_buf, sizeof(part_dsl_buf), "p%ld",
				 pv);
			part_str = part_dsl_buf;
		}
	}

	/*
	 * Init order:
	 *   1. FUSE setup (fuse_new, signal_handlers, mount, daemonize)
	 *   2. LKL kernel start  (AFTER fork — threads in child only)
	 *   3. Open disk(s) with anyfs_disk_open
	 *   4. Mode A: anyfs_disk_enter at startup; store LKL mount point
	 *      Mode B: optionally prefetch all partitions
	 *   5. FUSE event loop
	 */

	/* ── Step 1: FUSE setup ── */
	fuse = fuse_new(&args, &anyfs_fuse_ops, sizeof(anyfs_fuse_ops), NULL);
	if (!fuse) {
		ret = 1;
		goto out;
	}

	ret = fuse_set_signal_handlers(fuse_get_session(fuse));
	if (ret < 0)
		goto out_fuse_destroy;

	ret = fuse_mount(fuse, cli_mountpoint);
	if (ret < 0)
		goto out_remove_signals;

	fuse_opt_free_args(&args);

	ret = fuse_daemonize(cli_foreground);
	if (ret < 0)
		goto out_fuse_unmount;

	/* ── Step 2: LKL kernel start (after fork) ── */
	{
		AnyfsKernelOpts kopts = {
		    .mem_mb = cfg.mem_mb,
		    .loglevel = cfg.loglevel,
		};
		ret = anyfs_kernel_init(&kopts);
		if (ret < 0) {
			fprintf(stderr, "anyfs_kernel_init failed: %d\n", ret);
			goto out_fuse_unmount;
		}
		kernel_started = 1;
	}

	/* ── Step 3: Open disk(s) ── */
	{
		uint32_t disk_flags = ANYFS_BACKEND_QEMU;
		if (cfg.readonly)
			disk_flags |= ANYFS_DISK_READONLY;

		/* Build a combined image array: first image + extras */
		const char* all_images[MAX_DISKS];
		all_images[0] = cfg.image;
		for (int i = 0; i < g_nextra; i++)
			all_images[i + 1] = g_extra_images[i];

		AnyfsDisk* opened[MAX_DISKS] = {NULL};
		int n_total = 1 + g_nextra;
		if (anyfs_sesh_open_disks(opened, all_images, n_total,
					  disk_flags) < 0) {
			ret = 1;
			goto out_kernel_halt;
		}
		for (int i = 0; i < n_total; i++)
			g_disks[g_ndisks++].disk = opened[i];
	}

	/* ── Step 4a: Mode A — enter specified partition at startup ── */
	if (part_str) {
		g_mode_a = 1;

		AnyfsPath dsl;
		memset(&dsl, 0, sizeof(dsl));
		if (anyfs_path_dsl_parse(part_str, &dsl) < 0) {
			fprintf(stderr, "error: invalid -o part= path '%s'\n",
				part_str);
			ret = 1;
			goto out_disks_close;
		}

		int didx = dsl.disk_idx_set ? dsl.disk_idx : 0;
		if (didx < 0 || didx >= g_ndisks) {
			fprintf(stderr, "error: disk%d not registered\n", didx);
			anyfs_path_dsl_free(&dsl);
			ret = 1;
			goto out_disks_close;
		}
		unsigned int part_num =
		    dsl.comp[0].p; /* top-level slot for caching */

		char lkl_path[64];
		ret = anyfs_disk_enter_path(g_disks[didx].disk, dsl.comp,
					    dsl.n_comp, 0, lkl_path);
		if (ret < 0) {
			fprintf(
			    stderr,
			    "anyfs_disk_enter(disk%d, %s) failed: %d (%s)\n",
			    didx, part_str, ret,
			    anyfs_disk_fail_reason(g_disks[didx].disk,
						   part_num));
			anyfs_path_dsl_free(&dsl);
			goto out_disks_close;
		}
		anyfs_path_dsl_free(&dsl);
		snprintf(g_mount_point, sizeof(g_mount_point), "%s", lkl_path);
		/* Cache it in the PartCache too (top-level part_num only —
		 * nested paths are addressed by lkl_path directly). */
		if (part_num > 0 && part_num <= MAX_PARTS_PER_DISK)
			snprintf(
			    g_disks[didx].parts[part_num - 1].lkl_path,
			    sizeof(g_disks[didx].parts[part_num - 1].lkl_path),
			    "%s", lkl_path);

		fprintf(
		    stderr,
		    "anyfs-fuse: %s %s → %s, mounted at %s (backend: qemu)\n",
		    cfg.image, part_str, lkl_path, cli_mountpoint);

		/* ── Step 4b: Mode B — optional prefetch ── */
	} else if (cfg.prefetch) {
		for (int di = 0; di < g_ndisks; di++) {
			AnyfsPartInfo pbuf[MAX_PARTS_PER_DISK];
			size_t got = 0;
			anyfs_disk_list(g_disks[di].disk, pbuf,
					MAX_PARTS_PER_DISK, &got);
			for (size_t i = 0; i < got; i++) {
				unsigned int pn = pbuf[i].index;
				if (pn == 0 || pn > MAX_PARTS_PER_DISK)
					continue;
				char lkl_path[64];
				int r = anyfs_disk_enter(g_disks[di].disk, pn,
							 0, lkl_path);
				if (r < 0) {
					fprintf(stderr,
						"anyfs-fuse: prefetch "
						"disk%d/p%u failed: %d (%s)\n",
						di, pn, r,
						anyfs_disk_fail_reason(
						    g_disks[di].disk, pn));
				} else {
					snprintf(
					    g_disks[di].parts[pn - 1].lkl_path,
					    sizeof(g_disks[di]
						       .parts[pn - 1]
						       .lkl_path),
					    "%s", lkl_path);
					fprintf(stderr,
						"anyfs-fuse: prefetched "
						"disk%d/p%u → %s\n",
						di, pn, lkl_path);
				}
			}
		}
	}

	/* ── Step 5: FUSE event loop ── */
	if (!cli_singlethread)
		fprintf(stderr, "warning: multithreaded mode not supported, "
				"forcing single-threaded\n");

	ret = fuse_loop(fuse);
	if (ret < 0)
		fprintf(stderr, "fuse_loop error: %d\n", ret);

	/* ── Cleanup ── */
	lkl_sys_sync();

out_disks_close:
	for (int i = 0; i < g_ndisks; i++) {
		if (g_disks[i].disk) {
			anyfs_disk_close(g_disks[i].disk);
			g_disks[i].disk = NULL;
		}
	}

out_kernel_halt:
	if (kernel_started)
		anyfs_kernel_halt();

out_fuse_unmount:
	fuse_unmount(fuse);

out_remove_signals:
	fuse_remove_signal_handlers(fuse_get_session(fuse));

out_fuse_destroy:
	fuse_destroy(fuse);

out:
	free(cli_mountpoint);
	free(cfg.image);
	free(cfg.fstype);
	free(cfg.opts);
	free(cfg.part);
	for (int i = 0; i < g_nextra; i++)
		free(g_extra_images[i]);

	return ret;
}
