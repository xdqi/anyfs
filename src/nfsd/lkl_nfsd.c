// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lkl_nfsd.c - LKL-based NFSv4 server
 *
 * Boots a Linux Kernel Library instance with the NFS server (nfsd) bound
 * to lo inside LKL, mounts one or more disk images, and runs nfsd serving
 * NFSv4 only on port 2049. A host userspace TCP proxy (host_proxy.c)
 * bridges host *:port to LKL 127.0.0.1:2049, so libslirp/virtio-net are
 * not on the data path — same pattern as anyfs-ksmbd.
 *
 * Includes a mini "mountd" that handles sunrpc cache upcalls:
 *   - auth.unix.ip:  maps client IP -> auth domain "unix"
 *   - auth.unix.gid: maps UID -> supplementary GIDs
 *   - nfsd.fh:       maps (domain, fsid) -> export path
 *   - nfsd.export:   maps (domain, path) -> export flags
 *
 * Each --share becomes an NFS export rooted at /<name>.
 *
 * Build: meson compile -C build
 * Run:   ./anyfs-nfsd [options] <image>[?<query>] [<image>...] --share
 * [name=]path ... Test:  mount -t nfs4 localhost:/<name> /mnt -o port=20049
 *
 * Examples:
 *   anyfs-nfsd disk.img --share data=disk0/p1
 *   anyfs-nfsd boot.img data.qcow2 --share esp=disk0/p1 --share home=disk1/p1
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../core/path_dsl.h"
#include "../core/share_spec.h"
#include "../host_proxy/host_proxy.h"
#include "anyfs.h"
#include "anyfs_disk.h"
#include <lkl.h>

/* ── Compile-time limits ─────────────────────────────────────────────── */
#define MAX_DISKS 16
#define MAX_SHARES 32

/* ── Network defaults ─────────────────────────────────────────────────── */
/*
 * nfsd binds to lo inside LKL; a host-side userspace proxy bridges
 * host *:HOST_FWD_PORT -> LKL 127.0.0.1:NFS_PORT. libslirp / virtio-net
 * are not on the data path.
 */
#define NFS_PORT 2049
#define HOST_FWD_PORT 20049

/* ── Export descriptor ─────────────────────────────────────────────────── */
typedef struct {
	char name[64];	   /* NFS export name (client sees /<name>) */
	char lkl_path[64]; /* absolute LKL path returned by anyfs_disk_enter */
	/*
	 * bind_path is "/<name>" — the share's filesystem bind-mounted into
	 * LKL root so the NFSv4 pseudo-fs places it where the client expects.
	 * Without this, the share would land at /lklmnt/anyfs_d<N>_p<M> and a
	 * client mounting localhost:/<name> would LOOKUP "<name>" *inside* the
	 * share's root (matching e.g. Debian's /root user dir) instead of the
	 * partition root.
	 */
	char bind_path[80];
} ExportInfo;

static volatile int running = 1;

static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

/* Write a string to a file inside LKL */
static int lkl_write_file(const char* path, const char* data)
{
	int fd, ret;

	fd = lkl_sys_open(path, LKL_O_WRONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "lkl_sys_open(%s): %s\n", path,
			lkl_strerror(fd));
		return fd;
	}
	ret = lkl_sys_write(fd, data, strlen(data));
	lkl_sys_close(fd);
	if (ret < 0) {
		fprintf(stderr, "lkl_sys_write(%s): %s\n", path,
			lkl_strerror(ret));
		return ret;
	}
	return 0;
}

/*
 * Mini mountd: sunrpc cache upcall handler
 *
 * The kernel's sunrpc cache sends upcall queries via channel files.
 * We read those queries and write back responses, acting like rpc.mountd.
 *
 * Cache channels handled:
 *   auth.unix.ip:  query "class ip"        -> response "class ip expiry domain"
 *   auth.unix.gid: query "uid"             -> response "uid expiry 0"
 *   nfsd.fh:       query "domain type fsid"-> response "domain type fsid expiry
 * path" nfsd.export:   query "domain path"     -> response "domain path expiry
 * flags ..."
 *
 * Each export gets a unique 32-bit numeric fsid equal to its index (0-based).
 * The export path seen by the NFS client is "/<export.name>".
 */

/* Export table (set by main before starting handler) */
static ExportInfo g_exports[MAX_SHARES];
static int g_n_exports = 0;
static int g_read_only = 1;

#define NFSEXP_READONLY 0x0001
#define NFSEXP_INSECURE 0x0002
#define NFSEXP_ROOTSQUASH 0x0004
#define NFSEXP_ALLSQUASH 0x0008
#define NFSEXP_NOSUBTREECHECK 0x0400
#define NFSEXP_FSID 0x2000
#define NFSEXP_V4ROOT 0x10000

/* qword_get: parse one space-delimited word from mesg.
 * Handles \xHEX and \NNN octal escaping like the kernel does. */
static int qword_get_user(char** bpp, char* dest, int bufsize)
{
	char* bp = *bpp;
	int len = 0;

	while (*bp == ' ')
		bp++;
	if (*bp == '\0' || *bp == '\n') {
		*bpp = bp;
		return -1;
	}

	if (bp[0] == '\\' && bp[1] == 'x') {
		bp += 2;
		while (len < bufsize - 1) {
			int h, l;
			h = (*bp >= '0' && *bp <= '9')	 ? *bp - '0'
			    : (*bp >= 'a' && *bp <= 'f') ? *bp - 'a' + 10
			    : (*bp >= 'A' && *bp <= 'F') ? *bp - 'A' + 10
							 : -1;
			if (h < 0)
				break;
			bp++;
			l = (*bp >= '0' && *bp <= '9')	 ? *bp - '0'
			    : (*bp >= 'a' && *bp <= 'f') ? *bp - 'a' + 10
			    : (*bp >= 'A' && *bp <= 'F') ? *bp - 'A' + 10
							 : -1;
			if (l < 0)
				break;
			bp++;
			dest[len++] = (h << 4) | l;
		}
	} else {
		while (*bp != ' ' && *bp != '\n' && *bp != '\0' &&
		       len < bufsize - 1) {
			dest[len++] = *bp++;
		}
	}

	while (*bp == ' ')
		bp++;
	*bpp = bp;
	dest[len] = '\0';
	return len;
}

/* Handle auth.unix.ip upcall: "class ip\n" -> respond with domain "unix" */
static void handle_ip_map(int fd, char* query)
{
	char class[64], ip[64], resp[256];
	char* p = query;
	time_t expiry = time(NULL) + 3600 * 24 * 365;

	if (qword_get_user(&p, class, sizeof(class)) <= 0)
		return;
	if (qword_get_user(&p, ip, sizeof(ip)) <= 0)
		return;

	snprintf(resp, sizeof(resp), "%s %s %ld unix\n", class, ip,
		 (long)expiry);
	printf("[mountd] ip_map: %s %s -> unix\n", class, ip);
	lkl_sys_write(fd, resp, strlen(resp));
}

/* Handle auth.unix.gid upcall: "uid\n" -> respond with empty gid list */
static void handle_unix_gid(int fd, char* query)
{
	char uid_str[32], resp[128];
	char* p = query;
	time_t expiry = time(NULL) + 3600 * 24 * 365;

	if (qword_get_user(&p, uid_str, sizeof(uid_str)) <= 0)
		return;

	snprintf(resp, sizeof(resp), "%s %ld 0\n", uid_str, (long)expiry);
	printf("[mountd] unix_gid: uid %s -> 0 groups\n", uid_str);
	lkl_sys_write(fd, resp, strlen(resp));
}

/*
 * Handle nfsd.fh upcall: "domain fsidtype \xfsid\n" -> respond with path.
 *
 * We use FSID_NUM (type 1) with a 4-byte big-endian export index as the fsid.
 * fsid 0 is reserved for the kernel's synthesized NFSv4 pseudo-root — we
 * never claim it ourselves. Share N (0-based slot) uses fsid=N+1, so slot 0
 * → fsid bytes {0,0,0,1}, slot 1 → {0,0,0,2}, etc.
 *
 * The path in the response is the bind-mount path (/<share-name>) so the
 * pseudo-fs walks "<share-name>" to the export.
 */
static void handle_expkey(int fd, char* query)
{
	char domain[64], fsidtype_str[16], fsid_bytes[64], resp[512];
	char* p = query;
	int fsidtype, fsid_len;
	time_t expiry = time(NULL) + 3600 * 24 * 365;

	if (qword_get_user(&p, domain, sizeof(domain)) <= 0)
		return;
	if (qword_get_user(&p, fsidtype_str, sizeof(fsidtype_str)) <= 0)
		return;
	fsidtype = atoi(fsidtype_str);
	fsid_len = qword_get_user(&p, fsid_bytes, sizeof(fsid_bytes));
	if (fsid_len <= 0)
		return;

	/* We only handle FSID_NUM (type 1) with 4-byte fsid */
	if (fsidtype == 1 && fsid_len == 4) {
		/* Decode big-endian fsid (1-indexed) */
		unsigned int idx = ((unsigned char)fsid_bytes[0] << 24) |
				   ((unsigned char)fsid_bytes[1] << 16) |
				   ((unsigned char)fsid_bytes[2] << 8) |
				   (unsigned char)fsid_bytes[3];

		if (idx >= 1 && (int)idx <= g_n_exports) {
			int slot = (int)idx - 1;
			snprintf(resp, sizeof(resp),
				 "%s 1 \\x%02x%02x%02x%02x %ld %s\n", domain,
				 (idx >> 24) & 0xff, (idx >> 16) & 0xff,
				 (idx >> 8) & 0xff, idx & 0xff, (long)expiry,
				 g_exports[slot].bind_path);
			printf("[mountd] expkey: %s fsid=%u -> %s\n", domain,
			       idx, g_exports[slot].bind_path);
			lkl_sys_write(fd, resp, strlen(resp));
			return;
		}
	}

	/* Negative response */
	{
		char hex[128];
		int i, off = 0;
		off += snprintf(hex + off, sizeof(hex) - off, "\\x");
		for (i = 0; i < fsid_len && off < (int)sizeof(hex) - 2; i++)
			off += snprintf(hex + off, sizeof(hex) - off, "%02x",
					(unsigned char)fsid_bytes[i]);
		snprintf(resp, sizeof(resp), "%s %d %s %ld\n", domain, fsidtype,
			 hex, (long)expiry);
		printf("[mountd] expkey: %s fsid_type=%d -> NEGATIVE\n", domain,
		       fsidtype);
		lkl_sys_write(fd, resp, strlen(resp));
	}
}

/*
 * Handle nfsd.export upcall: "domain path\n" -> respond with export flags.
 *
 * The client-visible NFS path for export i is "/<name>", which is also the
 * server-side bind_path (a bind mount of the share's LKL fs). We match the
 * upcall against bind_path; fsid is the 1-based slot so 0 stays reserved
 * for the kernel's pseudo-root.
 */
static void handle_export(int fd, char* query)
{
	char domain[64], path[256], resp[512];
	char* p = query;
	time_t expiry = time(NULL) + 3600 * 24 * 365;

	if (qword_get_user(&p, domain, sizeof(domain)) <= 0)
		return;
	if (qword_get_user(&p, path, sizeof(path)) <= 0)
		return;

	/* Search for a matching export by bind path */
	for (int i = 0; i < g_n_exports; i++) {
		if (strcmp(path, g_exports[i].bind_path) == 0) {
			unsigned int flags = NFSEXP_INSECURE |
					     NFSEXP_NOSUBTREECHECK |
					     NFSEXP_FSID | NFSEXP_ALLSQUASH;
			if (g_read_only)
				flags |= NFSEXP_READONLY;
			/* fsid = slot index + 1 (big-endian 4 bytes); 0 reserved */
			unsigned int fsid = (unsigned int)(i + 1);
			snprintf(resp, sizeof(resp), "%s %s %ld %u 0 0 %u\n",
				 domain, path, (long)expiry, flags, fsid);
			printf("[mountd] export: %s %s -> flags=%#x fsid=%u\n",
			       domain, path, flags, fsid);
			lkl_sys_write(fd, resp, strlen(resp));
			return;
		}
	}

	/* Negative: domain path expiry (no flags) */
	snprintf(resp, sizeof(resp), "%s %s %ld\n", domain, path, (long)expiry);
	printf("[mountd] export: %s %s -> NEGATIVE\n", domain, path);
	lkl_sys_write(fd, resp, strlen(resp));
}

struct cache_channel {
	const char* path;
	void (*handler)(int fd, char* query);
};

static struct cache_channel channels[] = {
    {"/proc/net/rpc/auth.unix.ip/channel", handle_ip_map},
    {"/proc/net/rpc/auth.unix.gid/channel", handle_unix_gid},
    {"/proc/net/rpc/nfsd.fh/channel", handle_expkey},
    {"/proc/net/rpc/nfsd.export/channel", handle_export},
};
#define NUM_CHANNELS (sizeof(channels) / sizeof(channels[0]))

static void* cache_handler_thread(void* arg)
{
	(void)arg;
	int fds[NUM_CHANNELS];
	char buf[4096];

	for (int i = 0; i < (int)NUM_CHANNELS; i++) {
		fds[i] = lkl_sys_open(channels[i].path, LKL_O_RDWR, 0);
		if (fds[i] < 0) {
			fprintf(stderr, "[mountd] open %s: %s\n",
				channels[i].path, lkl_strerror(fds[i]));
			fds[i] = -1;
		} else {
			printf("[mountd] opened %s (fd=%d)\n", channels[i].path,
			       fds[i]);
		}
	}

	while (running) {
		struct lkl_pollfd pfds[NUM_CHANNELS];
		int nfds = 0;
		int fd_map[NUM_CHANNELS];

		for (int i = 0; i < (int)NUM_CHANNELS; i++) {
			if (fds[i] < 0)
				continue;
			pfds[nfds].fd = fds[i];
			pfds[nfds].events = LKL_POLLIN;
			pfds[nfds].revents = 0;
			fd_map[nfds] = i;
			nfds++;
		}

		if (nfds == 0) {
			usleep(100000);
			continue;
		}

		int ret = lkl_sys_poll(pfds, nfds, 200);
		if (ret <= 0)
			continue;

		for (int i = 0; i < nfds; i++) {
			if (!(pfds[i].revents & LKL_POLLIN))
				continue;

			int ch = fd_map[i];
			int n = lkl_sys_read(fds[ch], buf, sizeof(buf) - 1);
			if (n <= 0)
				continue;
			buf[n] = '\0';

			char* line = buf;
			while (*line) {
				char* nl = strchr(line, '\n');
				if (nl)
					*nl = '\0';
				if (strlen(line) > 0)
					channels[ch].handler(fds[ch], line);
				if (!nl)
					break;
				line = nl + 1;
			}
		}
	}

	for (int i = 0; i < (int)NUM_CHANNELS; i++) {
		if (fds[i] >= 0)
			lkl_sys_close(fds[i]);
	}
	return NULL;
}

/* Start nfsd with NFSv4-only configuration */
static int start_nfsd(void)
{
	int ret;

	ret = lkl_sys_mkdir("/proc/fs/nfsd", 0755);
	if (ret < 0 && ret != -LKL_EEXIST) {
		fprintf(stderr, "mkdir /proc/fs/nfsd: %s\n", lkl_strerror(ret));
		return ret;
	}

	ret = lkl_sys_mount("nfsd", "/proc/fs/nfsd", "nfsd", 0, NULL);
	if (ret < 0) {
		fprintf(stderr, "mount nfsd: %s\n", lkl_strerror(ret));
		return ret;
	}
	printf("Mounted nfsd control filesystem\n");

	ret = lkl_write_file("/proc/fs/nfsd/versions", "-3 +4\n");
	if (ret < 0)
		fprintf(stderr, "Warning: could not set versions\n");

	/* nfsd_get_default_max_blksize() auto-sizes against LKL's tiny
	 * totalram and bottoms out at 8 KiB, capping rsize at 8K. Force the
	 * upstream cap (1 MiB; NFSSVC_MAXBLKSIZE) before threads come up so
	 * clients can negotiate up. Must be written before the threads file. */
	ret = lkl_write_file("/proc/fs/nfsd/max_block_size", "1048576\n");
	if (ret < 0)
		fprintf(stderr, "Warning: could not set max_block_size\n");

	/* 8 server threads — one was a serialisation bottleneck under
	 * concurrent reads (8-way O_DIRECT stalled minutes before draining). */
	ret = lkl_write_file("/proc/fs/nfsd/threads", "8\n");
	if (ret < 0) {
		fprintf(stderr, "Failed to start nfsd threads: %s\n",
			lkl_strerror(ret));
		return ret;
	}
	printf("nfsd thread started\n");

	lkl_write_file("/proc/fs/nfsd/v4_end_grace", "Y\n");

	return 0;
}

/* ── Path-DSL helpers ────────────────────────────────────────────────── */
/* `--share name=path` parsing + literal-key warning live in
 * src/core/share_spec.{c,h} — same logic shared with anyfs-ksmbd. */

/* ── Usage ───────────────────────────────────────────────────────────── */
static void usage(FILE* f, const char* prog)
{
	fprintf(
	    f,
	    "Usage: %s [options] <image>[?<query>] [<image>...] --share "
	    "[name=]path ...\n"
	    "\n"
	    "Serve disk image(s) via NFSv4 over user-mode networking.\n"
	    "\n"
	    "Positional arguments:\n"
	    "  <image>[?<query>]  Disk image(s). First image is disk0, second "
	    "is disk1, etc.\n"
	    "                     The optional ?<query> is reserved for future "
	    "disk-level\n"
	    "                     credentials and is accepted but ignored in "
	    "v1.\n"
	    "\n"
	    "Options:\n"
	    "  --share [name=]path\n"
	    "                     Expose a partition as an NFS export.\n"
	    "                     'path' uses the canonical disk<N>/p<M> form "
	    "as printed\n"
	    "                     in the PATH column of `anyfs-lspart "
	    "<image>`:\n"
	    "                       disk0/p1        partition 1 of the first "
	    "image\n"
	    "                       disk1/p2        partition 2 of the second "
	    "image\n"
	    "                       p1              shortcut for disk0/p1 "
	    "(single-image only)\n"
	    "                     'name' is the NFS export name (client mounts "
	    "/<name>).\n"
	    "  -p N               [deprecated] Equivalent to --share "
	    "disk0/p<N> (single-image only).\n"
	    "  -P PORT            Host port for NFS (default: %d).\n"
	    "  -w                 Read-write export (default: read-only).\n"
	    "  -h, --help         Show this help.\n"
	    "\n"
	    "Examples:\n"
	    "  %s disk.img --share data=disk0/p1\n"
	    "  %s boot.img data.qcow2 --share esp=disk0/p1 --share "
	    "home=disk1/p1\n"
	    "  Discover paths first with: anyfs-lspart disk.img\n"
	    "  Then: mount -t nfs4 localhost:/<name> /mnt -o port=%d,vers=4\n",
	    prog, HOST_FWD_PORT, prog, prog, HOST_FWD_PORT);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char** argv)
{
	/* ── Argument storage ───────────────────────────────────────────────
	 */
	const char* disk_images[MAX_DISKS];
	int n_images = 0;
	char* share_specs[MAX_SHARES];
	int n_share_specs = 0;

	int host_port = HOST_FWD_PORT;
	int read_only = 1;
	int legacy_part = -1;

	/* ── Option parsing ─────────────────────────────────────────────────
	 */
	static const struct option long_opts[] = {
	    {"share", required_argument, NULL, 1000},
	    {"help", no_argument, NULL, 'h'},
	    {NULL, 0, NULL, 0}};

	int opt;
	/* No leading '+': let GNU getopt permute so --share can appear after
	 * positional <image> args (which matches the documented usage). */
	while ((opt = getopt_long(argc, argv, "hp:P:w", long_opts, NULL)) !=
	       -1) {
		switch (opt) {
		case 1000: /* --share */
			if (n_share_specs >= MAX_SHARES) {
				fprintf(
				    stderr,
				    "error: too many --share flags (max %d)\n",
				    MAX_SHARES);
				return 1;
			}
			share_specs[n_share_specs++] = optarg;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		case 'p':
			fprintf(stderr,
				"warning: -p N is deprecated. Use '--share "
				"p%s' instead.\n",
				optarg);
			legacy_part = atoi(optarg);
			break;
		case 'P':
			host_port = atoi(optarg);
			break;
		case 'w':
			read_only = 0;
			break;
		default:
			usage(stderr, argv[0]);
			return 1;
		}
	}

	/* ── Collect positional disk images ──────────────────────────────── */
	for (; optind < argc; optind++) {
		if (n_images >= MAX_DISKS) {
			fprintf(stderr,
				"error: too many disk images (max %d)\n",
				MAX_DISKS);
			return 1;
		}
		disk_images[n_images++] = argv[optind];
	}

	if (n_images == 0) {
		fprintf(stderr, "error: at least one disk image is required\n");
		usage(stderr, argv[0]);
		return 1;
	}

	/* ── Back-compat: -p N → implicit --share ───────────────────────────
	 */
	char legacy_spec[32] = {0};
	if (legacy_part >= 0) {
		if (n_images > 1) {
			fprintf(
			    stderr,
			    "error: -p N is only supported in single-disk "
			    "mode. "
			    "Use --share disk<N>/p<M> in multi-disk mode.\n");
			return 1;
		}
		if (legacy_part == 0)
			snprintf(legacy_spec, sizeof(legacy_spec), "p0");
		else
			snprintf(legacy_spec, sizeof(legacy_spec), "p%d",
				 legacy_part);
		if (n_share_specs < MAX_SHARES)
			share_specs[n_share_specs++] = legacy_spec;
	}

	/* ── Require at least one share ─────────────────────────────────────
	 */
	if (n_share_specs == 0) {
		fprintf(
		    stderr,
		    "error: no shares specified. "
		    "Use '--share [name=]disk<N>/p<M>' (e.g. --share disk0/p1) "
		    "to expose a partition.\n"
		    "Run 'anyfs-lspart %s' to see partitions in this image.\n"
		    "Run '%s -h' for help.\n",
		    n_images > 0 ? disk_images[0] : "<image>", argv[0]);
		return 1;
	}

	/* ── Signals / stdio ────────────────────────────────────────────────
	 */
	setbuf(stdout, NULL);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	/* ── 1. Boot LKL kernel ─────────────────────────────────────────────
	 */
	AnyfsKernelOpts kern_opts = {.mem_mb = 0 /* anyfs default (32M) */,
				     .loglevel = 4};
	int ret = anyfs_kernel_init(&kern_opts);
	if (ret) {
		fprintf(stderr, "Failed to start kernel\n");
		return 1;
	}
	printf("LKL kernel started (nfsd built-in)\n");

	/*
	 * No virtio-net / slirp: the data path is host TCP -> host_proxy
	 * threads -> lkl_sys_read/write -> LKL TCP on lo. We just need lo
	 * up so nfsd's listener binds to it. (lo is auto-up after boot,
	 * but the call is idempotent and documents intent.)
	 */
	lkl_if_up(1); /* loopback is always ifindex 1 in LKL */

	/* ── 4. Open disk images ────────────────────────────────────────────
	 */
	AnyfsDisk* disks[MAX_DISKS] = {NULL};
	int n_open = 0;

	for (int i = 0; i < n_images; i++) {
		const char* img = disk_images[i];
		char img_clean[512];
		const char* qmark = strchr(img, '?');
		if (qmark) {
			size_t len = (size_t)(qmark - img);
			if (len >= sizeof(img_clean))
				len = sizeof(img_clean) - 1;
			memcpy(img_clean, img, len);
			img_clean[len] = '\0';
			img = img_clean;
		}

		uint32_t dflags = read_only ? ANYFS_DISK_READONLY : 0;
		if (anyfs_disk_open(img, dflags, &disks[i]) < 0 || !disks[i]) {
			fprintf(stderr, "Failed to open disk image '%s'\n",
				disk_images[i]);
			goto halt;
		}
		n_open++;
		printf("Opened disk%d: %s (id=%d)\n", i, img,
		       anyfs_disk_id(disks[i]));
	}

	/* ── 5. Resolve --share specs to LKL paths ───────────────────────── */
	for (int si = 0; si < n_share_specs; si++) {
		const char* name_arg;
		const char* path_arg;
		char spec_copy[256];
		strncpy(spec_copy, share_specs[si], sizeof(spec_copy) - 1);
		spec_copy[sizeof(spec_copy) - 1] = '\0';

		anyfs_share_split(spec_copy, &name_arg, &path_arg);

		/* Back-compat: bare integer → p<N> */
		char rebased[64];
		if (isdigit((unsigned char)path_arg[0])) {
			if (n_images > 1) {
				fprintf(stderr,
					"error: bare integer share '%s' is "
					"only valid in "
					"single-disk mode.\n",
					path_arg);
				goto halt;
			}
			snprintf(rebased, sizeof(rebased), "p%s", path_arg);
			path_arg = rebased;
			fprintf(stderr,
				"warning: --share %s treated as --share %s "
				"(use 'p<N>' to suppress this warning)\n",
				share_specs[si], rebased);
		}

		/* In single-disk mode, auto-prefix missing disk<N>/ with disk0/
		 */
		char prefixed[256];
		if (n_images == 1 && strncmp(path_arg, "disk", 4) != 0) {
			snprintf(prefixed, sizeof(prefixed), "disk0/%s",
				 path_arg);
			path_arg = prefixed;
		}

		/* Parse via path DSL */
		AnyfsPath ap;
		memset(&ap, 0, sizeof(ap));
		if (anyfs_path_dsl_parse(path_arg, &ap) < 0) {
			fprintf(stderr,
				"error: --share path '%s' is not a valid path "
				"DSL string.\n",
				path_arg);
			goto halt;
		}

		/* Multi-disk mode: path must have explicit disk<N>/ prefix */
		if (n_images > 1 && !ap.disk_idx_set) {
			fprintf(stderr,
				"error: path '%s' must start with diskN/ in "
				"multi-disk mode "
				"(have %d images: disk0..disk%d).\n",
				share_specs[si], n_images, n_images - 1);
			anyfs_path_dsl_free(&ap);
			goto halt;
		}

		int disk_idx = ap.disk_idx;

		if (disk_idx >= n_images) {
			fprintf(stderr,
				"error: disk%d not registered (only "
				"disk0..disk%d available).\n",
				disk_idx, n_images - 1);
			anyfs_path_dsl_free(&ap);
			goto halt;
		}

		if (ap.n_comp == 0) {
			fprintf(stderr,
				"error: --share path '%s' has no partition "
				"component.\n",
				path_arg);
			anyfs_path_dsl_free(&ap);
			goto halt;
		}

		anyfs_share_warn_literal_key(&ap,
					     name_arg ? name_arg : path_arg);

		/* v2: walk every component. Containers are entered, then we
		 * descend into their children; final segment must be FS. */
		uint32_t eflags = read_only ? ANYFS_DISK_READONLY : 0;
		char lkl_path[64];
		ret = anyfs_disk_enter_path(disks[disk_idx], ap.comp, ap.n_comp,
					    eflags, lkl_path);
		if (ret < 0) {
			const char* reason = anyfs_disk_fail_reason(
			    disks[disk_idx], ap.comp[0].p);
			fprintf(
			    stderr,
			    "error: cannot enter %s: %s\n"
			    "Containers (LVM_PV, LUKS, nested partition table) "
			    "require\n"
			    "either a credential (`?keyref=`) or v3 support.\n"
			    "Use 'anyfs-lspart' to discover the canonical leaf "
			    "path.\n",
			    path_arg, reason ? reason : lkl_strerror(ret));
			anyfs_path_dsl_free(&ap);
			goto halt;
		}

		char canonical[160];
		int co =
		    snprintf(canonical, sizeof(canonical), "disk%d", disk_idx);
		for (size_t ci = 0; ci < ap.n_comp; ci++) {
			int n = snprintf(canonical + co, sizeof(canonical) - co,
					 "_p%u", ap.comp[ci].p);
			if (n < 0 || (size_t)n >= sizeof(canonical) - co)
				break;
			co += n;
		}

		ExportInfo* ex = &g_exports[g_n_exports];
		if (name_arg && *name_arg) {
			strncpy(ex->name, name_arg, sizeof(ex->name) - 1);
			ex->name[sizeof(ex->name) - 1] = '\0';
		} else {
			anyfs_share_auto_name(canonical, ex->name,
					      sizeof(ex->name));
		}
		strncpy(ex->lkl_path, lkl_path, sizeof(ex->lkl_path) - 1);
		ex->lkl_path[sizeof(ex->lkl_path) - 1] = '\0';

		printf("Export [%d] /%s -> %s (%s)\n", g_n_exports, ex->name,
		       ex->lkl_path, canonical);
		anyfs_path_dsl_free(&ap);
		g_n_exports++;
	}

	if (g_n_exports == 0) {
		fprintf(stderr, "No exports could be mounted.\n");
		goto halt;
	}

	g_read_only = read_only;

	/* ── 5.5 Bind each share to /<name> for the NFSv4 pseudo-fs ─────────
	 * The kernel synthesizes the NFSv4 pseudo-root from real export paths.
	 * A share landed at /lklmnt/anyfs_d0_p1 would be reachable only via
	 * /lklmnt/anyfs_d0_p1, while clients want localhost:/<name>. Bind-mount
	 * the share root at /<name> so the pseudo-fs places <name> in the root
	 * directory listing and LOOKUP <name> resolves to the partition root
	 * (not /<name> *inside* the partition).
	 */
	static const char* const reserved[] = {"proc", "sys", "lklmnt", "dev",
					       NULL};
	for (int i = 0; i < g_n_exports; i++) {
		ExportInfo* ex = &g_exports[i];
		for (int r = 0; reserved[r]; r++) {
			if (strcmp(ex->name, reserved[r]) == 0) {
				fprintf(stderr,
					"error: share name '%s' collides with "
					"a reserved LKL root path.\n",
					ex->name);
				goto halt;
			}
		}
		/* snprintf into a local buffer first to dodge -Wrestrict — both
		 * dest and src live inside the same ExportInfo struct. */
		char tmp_bind[80];
		snprintf(tmp_bind, sizeof(tmp_bind), "/%s", ex->name);
		memcpy(ex->bind_path, tmp_bind, sizeof(ex->bind_path));
		int mret = lkl_sys_mkdir(ex->bind_path, 0755);
		if (mret < 0 && mret != -LKL_EEXIST) {
			fprintf(stderr, "mkdir %s: %s\n", ex->bind_path,
				lkl_strerror(mret));
			goto halt;
		}
		mret = lkl_sys_mount(ex->lkl_path, ex->bind_path, NULL,
				     LKL_MS_BIND, NULL);
		if (mret < 0) {
			fprintf(stderr, "bind %s -> %s: %s\n", ex->lkl_path,
				ex->bind_path, lkl_strerror(mret));
			goto halt;
		}
		printf("Pseudo-fs: %s -> %s\n", ex->bind_path, ex->lkl_path);
	}

	/* ── 6. Start nfsd ──────────────────────────────────────────────────
	 */
	if (start_nfsd() < 0)
		goto halt;

	/* ── 7. Start cache upcall handler thread (mini mountd) ─────────────
	 */
	pthread_t cache_tid;
	if (pthread_create(&cache_tid, NULL, cache_handler_thread, NULL) != 0) {
		perror("pthread_create");
		goto halt;
	}

	/*
	 * nfsd is now listening on lo:NFS_PORT inside LKL. Start the host
	 * TCP proxy that bridges host:host_port to it.
	 */
	if (host_proxy_start((uint16_t)host_port, NFS_PORT) < 0) {
		fprintf(stderr, "host_proxy_start failed\n");
		goto halt;
	}

	printf("\n=== NFSv4 server ready ===\n");
	for (int i = 0; i < g_n_exports; i++)
		printf("  mount -t nfs4 localhost:/%s /mnt -o port=%d,vers=4\n",
		       g_exports[i].name, host_port);
	printf("Press Ctrl+C to stop.\n\n");

	/* ── 8. Serve until interrupted ─────────────────────────────────────
	 */
	while (running) {
		usleep(100000);
	}

	printf("\nShutting down...\n");
	host_proxy_stop();
	pthread_join(cache_tid, NULL);

halt:
	for (int i = 0; i < n_open; i++) {
		if (disks[i])
			anyfs_disk_close(disks[i]);
	}
	anyfs_kernel_halt();
	printf("Done\n");
	return 0;
}
