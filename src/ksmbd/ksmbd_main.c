// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lkl_ksmbd.c - LKL-based SMB3 server using ksmbd + ksmbd-tools
 *
 * Boots a Linux Kernel Library instance with the ksmbd module, binds
 * the in-kernel SMB listener to lo, mounts one or more disk images,
 * and runs the ksmbd-tools IPC daemon to serve SMB3 shares. A host
 * userspace TCP proxy (host_proxy.c) bridges host *:port to the
 * LKL-internal 127.0.0.1:445, so libslirp is not on the data path.
 *
 * Build: meson compile -C build
 * Run:   ./anyfs-ksmbd [options] <image>[?<query>] [<image>...] --share
 * name=path ... Test:  smbclient //localhost/esp -U guest%guest --port=4455
 *
 * Examples:
 *   anyfs-ksmbd disk.img --share data=disk0/p1
 *   anyfs-ksmbd boot.img data.qcow2 --share esp=disk0/p1 --share home=disk1/p1
 *   anyfs-ksmbd disk.img -P 4450          (back-compat: auto share p0
 * whole-disk) anyfs-ksmbd disk.img -p 2             (deprecated: equivalent to
 * --share disk0/p2)
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/anyfs_path.h"
#include "../core/anyfs_share.h"
#include "../host_proxy/host_proxy.h"
#include "anyfs.h"
#include "anyfs_session.h"
#include "fastsync_win.h"
#include <lkl.h>

#include <config_parser.h>
#include <ipc.h>
#include <linux/ksmbd_server.h>
#include <management/session.h>
#include <management/share.h>
#include <management/spnego.h>
#include <management/tree_conn.h>
#include <management/user.h>
#include <rpc.h>
#include <tools.h>
#include <worker.h>

/* ── Compile-time limits ─────────────────────────────────────────────── */
/* (ANYFS_MAX_DISKS / ANYFS_MAX_SHARES / ANYFS_LKL_PATH_MAX are in anyfs_disk.h)
 */

/* ── Network defaults ─────────────────────────────────────────────────── */
/*
 * ksmbd binds to lo inside LKL; a host-side userspace proxy bridges
 * host *:HOST_FWD_PORT -> LKL 127.0.0.1:SMB_PORT. libslirp is not on
 * the data path.
 */
#define SMB_PORT 445
#define HOST_FWD_PORT 4455
#define GUEST_USER "guest"

/* ── Share descriptor ─────────────────────────────────────────────────── */
typedef struct {
	char name[64]; /* SMB share name (section header in smb.conf) */
	char lkl_path[ANYFS_LKL_PATH_MAX]; /* absolute LKL path returned by
					      anyfs_disk_enter */
} ShareInfo;

static volatile int running = 1;

static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

/* ── Runtime-tunable knobs ───────────────────────────────────────────── */
/*
 * Per-connection peak kernel memory (slab + in-flight kvzalloc) is roughly
 * 10-16 MiB at smb2_max_read=1 MiB with smbclient's ~8 in-flight reads. So
 * the relevant ceilings the user might want to dial down are:
 *   --max-read   = ksmbd smb2_max_read / smb2_max_write / smb2_max_trans
 *                  / smbd_max_io_size  (controls the per-IO buffer kvzalloc)
 *   --max-conn   = ksmbd max_connections (hard cap on simultaneous TCP)
 *   --max-credits= ksmbd smb2_max_credits (per-connection in-flight ceiling)
 *   --mem-mb     = LKL kernel arena size (mem= boot arg)
 *
 * Defaults below match the previous hard-coded values (1 MiB IO, 128 conn,
 * 8192 credits) and the new mem=32M default from anyfs_kernel_init.
 */
typedef struct {
	int max_read;	 /* bytes; <=0 ⇒ default */
	int max_conn;	 /* connections; <=0 ⇒ default */
	int max_credits; /* credits;  <=0 ⇒ default */
} KsmbdLimits;

#define DEF_MAX_READ 1048576
#define DEF_MAX_CONN 128
#define DEF_MAX_CREDITS 8192

/* ── Global config setup ─────────────────────────────────────────────── */
static void setup_global_conf(const KsmbdLimits* lim)
{
	int max_read = lim->max_read > 0 ? lim->max_read : DEF_MAX_READ;
	int max_conn = lim->max_conn > 0 ? lim->max_conn : DEF_MAX_CONN;
	int max_credits =
	    lim->max_credits > 0 ? lim->max_credits : DEF_MAX_CREDITS;

	memset(&global_conf, 0, sizeof(global_conf));

	global_conf.tcp_port = SMB_PORT;
	global_conf.ipc_timeout = 30;
	global_conf.deadtime = 0;
	global_conf.file_max = 10000;
	/*
	 * IO ceiling. Default 1 MiB matches the per-IO kvzalloc size ksmbd
	 * allocates for SMB2_READ. Each in-flight read therefore costs roughly
	 * this much; lower it under tight LKL mem= budgets. `smbd_max_io_size`
	 * is the RDMA path's ceiling but ksmbd also clamps non-RDMA buffer
	 * alloc against it.
	 */
	global_conf.smb2_max_read = max_read;
	global_conf.smb2_max_write = max_read;
	global_conf.smb2_max_trans = max_read;
	global_conf.smb2_max_credits = max_credits;
	global_conf.smbd_max_io_size = max_read;
	global_conf.max_connections = max_conn;
	global_conf.share_fake_fscaps = 0;
	global_conf.server_signing = 0;

	/*
	 * sm_check_sessions_capacity() does an atomic decrement on this. The
	 * normal smb.conf path seeds it to 1024 via add_group_global_conf() →
	 * process_global_conf_kv() ("max active sessions = 1024"). We skip
	 * that path, so without an explicit seed the very first tree connect
	 * decrements 0 → -EINVAL → TOO_MANY_SESSIONS → client sees
	 * NT_STATUS_ACCESS_DENIED on tree connect. Match the upstream
	 * default.
	 */
	global_conf.sessions_cap = 1024;

	global_conf.server_min_protocol = g_strdup("SMB2_10");
	global_conf.server_max_protocol = g_strdup("SMB3_11");
	global_conf.netbios_name = g_strdup("LKLSMB");
	global_conf.server_string = g_strdup("LKL ksmbd Server");
	global_conf.work_group = g_strdup("WORKGROUP");
	global_conf.guest_account = g_strdup(GUEST_USER);

	global_conf.map_to_guest = KSMBD_CONF_MAP_TO_GUEST_BAD_USER;

	/*
	 * Bind the in-kernel listener only to lo. The host-side TCP proxy
	 * (see host_proxy.c) connects to 127.0.0.1:445 from outside LKL,
	 * so binding lo is sufficient — no other netdev exists.
	 */
	global_conf.interfaces = g_strsplit("lo", ",", -1);
	global_conf.bind_interfaces_only = 1;
}

/* ── In-memory ksmbd-tools configuration ─────────────────────────────── */

/*
 * Default share options applied to every --share. Built with mutable buffers
 * because cp_parse_external_smbconf_group's helpers (is_a_key_value) may
 * write a NUL into the value to strip trailing whitespace.
 *
 * `force user = root` is *intentionally absent*: that smb.conf knob would
 * route through ksmbd-tools' force_user() → getpwnam("root"), which depends
 * on /etc/passwd and is unreliable across platforms. We achieve the same
 * effect — every SMB op runs as uid 0 so the mounted FS's UNIX perm bits
 * never block us — by stamping force_uid/force_gid directly on each share
 * in finalize_in_memory_config(). See the comment there.
 */
static void add_share_group(const char* name, const char* lkl_path)
{
	char path_opt[160];
	char guest_opt[] = "guest ok = yes";
	char ro_opt[] = "read only = yes";
	char browse_opt[] = "browseable = yes";
	char crossmnt_opt[] = "crossmnt = yes";
	/*
	 * Override ksmbd-tools defaults that don't fit a read-only disk-reader:
	 *   - oplocks: we can't take lease-break callbacks back through the
	 * host_proxy splice and the RO image never changes, so oplocks just
	 * add state.
	 *   - store dos attributes: would chase user.DOSATTRIB xattr on every
	 *     stat; the on-disk FSes we mount don't have it, every lookup pays
	 *     a getxattr -> -ENODATA round trip.
	 *   - hide dot files: default yes makes `.config/`, `.bashrc` etc.
	 *     invisible to Windows clients. We're a forensic disk reader — the
	 *     user wants to see everything.
	 */
	char oplocks_opt[] = "oplocks = no";
	char dosattr_opt[] = "store dos attributes = no";
	char hidedot_opt[] = "hide dot files = no";

	snprintf(path_opt, sizeof(path_opt), "path = %s", lkl_path);

	char* opts[] = {
	    path_opt,	 guest_opt,   ro_opt,	   browse_opt, crossmnt_opt,
	    oplocks_opt, dosattr_opt, hidedot_opt, NULL,
	};
	cp_parse_external_smbconf_group((char*)name, opts);
}

/*
 * After the parser has been populated with [share] groups via
 * cp_parse_external_smbconf_group, push each non-global group through
 * shm_add_new_share. This is the part finalize_smbconf_parser would have
 * done; we replicate it here because that function is static and assumes
 * it owns the global-conf processing — which we already did via
 * setup_global_conf().
 */
static int finalize_in_memory_config(void)
{
	GHashTableIter iter;
	gpointer key, value;
	int ret = 0;

	g_hash_table_iter_init(&iter, parser.groups);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		struct smbconf_group* g = value;
		if (g == parser.global)
			continue;
		if (shm_add_new_share(g)) {
			pr_err("shm_add_new_share failed for [%s]\n", g->name);
			ret = -1;
			continue;
		}

		/*
		 * Stamp uid/gid 0 on every share — the cross-platform
		 * equivalent of `force user = root`. anyfs-ksmbd is a pure
		 * disk-reader: the mounted FS's UNIX perm bits exist on the
		 * image, not in our security model, and we must surface every
		 * file to the SMB client regardless. Doing it here (instead of
		 * via the conf knob) avoids force_user()'s getpwnam()
		 * dependency, which behaves differently on Linux glibc, musl
		 * static builds, and wine (no /etc/passwd at all).
		 */
		struct ksmbd_share* sh = shm_lookup_share(g->name);
		if (sh) {
			sh->force_uid = 0;
			sh->force_gid = 0;
			put_ksmbd_share(sh);
		}
	}
	cp_smbconf_parser_destroy();
	return ret;
}

/*
 * Parse a host-side smb.conf into memory and replay each section through
 * cp_parse_external_smbconf_group. Avoids the ksmbd-tools mmap parser
 * entirely, which trips on Windows-style CRLF (the parser splits on '\n'
 * without stripping '\r' and rejects `[global]\r` as an invalid entry).
 *
 * Returns 0 on success, -1 on failure.
 */
static int parse_host_smbconf(const char* path)
{
	FILE* fp = fopen(path, "rb");
	if (!fp) {
		perror("fopen smbconf");
		return -1;
	}

	char line[1024];
	char section[128] = {0};
	/* Up to ANYFS_MAX_SHARES sections * up to 32 options per section.
	 * Strings are mutable (cp_parse_external_smbconf_group can
	 * NUL-terminate values).
	 */
	enum { MAX_OPTS = 32 };
	char* opts[MAX_OPTS + 1];
	int n_opts = 0;
	int have_section = 0;

	/* Stash the current section's option strings so we can pass them all
	 * at once when we hit the next section header (or EOF). */
	char* stash[MAX_OPTS];
	for (int i = 0; i < MAX_OPTS; i++)
		stash[i] = NULL;

	while (fgets(line, sizeof(line), fp)) {
		/* Strip CR/LF and trailing whitespace */
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
				 line[n - 1] == ' ' || line[n - 1] == '\t'))
			line[--n] = '\0';

		/* Skip leading whitespace */
		char* p = line;
		while (*p == ' ' || *p == '\t')
			p++;

		/* Blank / comment lines */
		if (*p == '\0' || *p == ';' || *p == '#')
			continue;

		/* Section header */
		if (*p == '[') {
			char* end = strchr(p, ']');
			if (!end) {
				fprintf(
				    stderr,
				    "warning: malformed section header: %s\n",
				    p);
				continue;
			}
			*end = '\0';
			const char* name = p + 1;

			/* Flush the previous section */
			if (have_section) {
				opts[n_opts] = NULL;
				cp_parse_external_smbconf_group(section, opts);
				for (int i = 0; i < n_opts; i++) {
					free(stash[i]);
					stash[i] = NULL;
				}
				n_opts = 0;
			}

			strncpy(section, name, sizeof(section) - 1);
			section[sizeof(section) - 1] = '\0';
			have_section = 1;
			continue;
		}

		/* Key = value line. Must be inside a section. */
		if (!have_section) {
			fprintf(stderr,
				"warning: option before any section: %s\n", p);
			continue;
		}
		if (!strchr(p, '=')) {
			fprintf(stderr, "warning: ignored non-kv line: %s\n",
				p);
			continue;
		}
		if (n_opts >= MAX_OPTS) {
			fprintf(stderr,
				"warning: too many options in [%s], extra "
				"ignored\n",
				section);
			continue;
		}
		stash[n_opts] = strdup(p);
		if (!stash[n_opts]) {
			fprintf(stderr, "strdup failed\n");
			fclose(fp);
			return -1;
		}
		opts[n_opts] = stash[n_opts];
		n_opts++;
	}

	/* Flush the last section */
	if (have_section) {
		opts[n_opts] = NULL;
		cp_parse_external_smbconf_group(section, opts);
		for (int i = 0; i < n_opts; i++)
			free(stash[i]);
	}

	fclose(fp);
	return 0;
}

/*
 * Set up ksmbd-tools in-memory: registers the guest user, builds one share
 * group per ShareInfo (and/or pulls extra sections out of the host conf if
 * supplied), then brings up the MOUNTD subsystems (sm/rpc/ipc/spnego/wp).
 *
 * Mirrors load_config(MOUNTD) without ever touching a config file inside
 * LKL — avoids the mmap/CRLF cross-platform fragility entirely.
 */
static int setup_ksmbd_config(const ShareInfo* shares, int n_shares,
			      const char* host_smbconf)
{
	extern tool_main_fn* tool_main;
	tool_main = mountd_main; /* TOOL_IS_MOUNTD must be true */

	usm_init();
	usm_remove_all_users();
	shm_init();
	shm_remove_all_shares();
	cp_smbconf_parser_init();

	/* global_conf has already been populated by setup_global_conf(); the
	 * side-effect we still need from finalize_smbconf_parser is registering
	 * the guest account user. */
	if (usm_add_guest_account(global_conf.guest_account)) {
		pr_err("usm_add_guest_account failed\n");
		return -1;
	}

	/* Programmatic shares from --share. */
	for (int i = 0; i < n_shares; i++)
		add_share_group(shares[i].name, shares[i].lkl_path);

	/* Optional: extra sections from a host-side smb.conf. */
	if (host_smbconf && parse_host_smbconf(host_smbconf) < 0)
		return -1;

	if (finalize_in_memory_config() < 0)
		return -1;

	sm_init();
	rpc_init();
	ipc_init();
	spnego_init();
	wp_init();
	return 0;
}

/* ── Path-DSL helpers ────────────────────────────────────────────────── */
/* `--share name=path` parsing + literal-key warning live in
 * src/core/share_spec.{c,h} — same logic shared with anyfs-nfsd. */

/* ── Usage ───────────────────────────────────────────────────────────── */
static void usage(FILE* f, const char* prog)
{
	fprintf(
	    f,
	    "Usage: %s [options] <image>[?<query>] [<image>...] --share "
	    "[name=]path ...\n"
	    "\n"
	    "Serve disk image(s) via SMB3 (KSMBD) over user-mode networking.\n"
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
	    "                     Expose a partition as an SMB share.\n"
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
	    "                     'name' is the SMB share name (default: "
	    "auto-derived from path).\n"
	    "  -c FILE            Use FILE as ksmbd.conf (overrides built-in "
	    "config).\n"
	    "                     [deprecated: prefer --share; still "
	    "honoured]\n"
	    "  -d                 Enable debug logging.\n"
	    "  -p N               [deprecated] Equivalent to --share p<N> "
	    "(single-disk only).\n"
	    "  -P PORT            Host port for SMB (default: %d).\n"
	    "\n"
	    "Resource limits (raise/lower to trade memory for concurrency):\n"
	    "  --mem-mb N         LKL kernel arena size in MiB (default: 32).\n"
	    "                     Peak kernel use is ~10-16 MiB per concurrent "
	    "SMB\n"
	    "                     session at the default 1 MiB IO size, plus "
	    "page\n"
	    "                     cache. Increase if you hit "
	    "STATUS_INVALID_HANDLE\n"
	    "                     under heavy concurrency.\n"
	    "  --max-read BYTES   Max SMB2 read/write/transact (default: %d).\n"
	    "                     Lower this (e.g. 262144) to shrink the "
	    "per-IO\n"
	    "                     kvzalloc buffer; higher values give better "
	    "single-\n"
	    "                     stream throughput at higher memory cost.\n"
	    "  --max-conn N       Max simultaneous SMB connections (default: "
	    "%d).\n"
	    "  --max-credits N    Max SMB2 credits per connection (default: "
	    "%d).\n"
	    "                     Caps the number of in-flight requests one "
	    "client\n"
	    "                     can have outstanding.\n"
	    "  --busy-spin        Replace host_proxy's blocking poll() with a\n"
	    "                     non-blocking poll(0)+yield loop. Eliminates "
	    "the\n"
	    "                     wineserver IPC round-trip on every wake, at "
	    "the\n"
	    "                     cost of pegging two CPUs. Useful only under "
	    "wine.\n"
	    "  --no-fast-sync     [Windows] Disable "
	    "WaitOnAddress/CRITICAL_SECTION\n"
	    "                     overrides for LKL sem/mutex; revert to "
	    "stock\n"
	    "                     CreateSemaphore/WaitForSingleObject. The "
	    "fast\n"
	    "                     impl is on by default on Windows because it\n"
	    "                     skips wineserver IPC on every sched wake.\n"
	    "  -h, --help         Show this help.\n"
	    "\n"
	    "Examples:\n"
	    "  %s disk.img --share data=disk0/p1\n"
	    "  %s disk.img -P 4450\n"
	    "  %s boot.img data.qcow2 --share esp=disk0/p1 --share "
	    "home=disk1/p1\n"
	    "  %s disk.img --share disk0/p1 --mem-mb 128 --max-read 262144   # "
	    "low-mem profile\n"
	    "  Discover paths first with: anyfs-lspart disk.img\n"
	    "  Then: smbclient //localhost/<name> -U guest%%guest --port=%d\n",
	    prog, HOST_FWD_PORT, DEF_MAX_READ, DEF_MAX_CONN, DEF_MAX_CREDITS,
	    prog, prog, prog, prog, HOST_FWD_PORT);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char** argv)
{
	/* ── Argument storage ───────────────────────────────────────────────
	 */
	const char* disk_images[ANYFS_MAX_DISKS];
	int n_images = 0;
	/* raw share specs accumulated during getopt_long */
	char* share_specs[ANYFS_MAX_SHARES];
	int n_share_specs = 0;

	const char* config_file = NULL;
	int log_level = PR_INFO;
	int host_port = HOST_FWD_PORT;
	int legacy_part = -1;		/* -p N, -1 = not set */
	int mem_mb = 0;			/* 0 ⇒ anyfs_kernel_init default (32) */
	KsmbdLimits limits = {0, 0, 0}; /* 0 ⇒ defaults */
#ifdef _WIN32
	int fast_sync = 1; /* on by default on Windows */
#else
	int fast_sync = 0; /* no-op on POSIX anyway */
#endif

	/* ── Option parsing ─────────────────────────────────────────────────
	 */
	static const struct option long_opts[] = {
	    {"share", required_argument, NULL, 1000},
	    {"mem-mb", required_argument, NULL, 1001},
	    {"max-read", required_argument, NULL, 1002},
	    {"max-conn", required_argument, NULL, 1003},
	    {"max-credits", required_argument, NULL, 1004},
	    {"busy-spin", no_argument, NULL, 1005},
	    {"no-fast-sync", no_argument, NULL, 1006},
	    {"help", no_argument, NULL, 'h'},
	    {NULL, 0, NULL, 0}};

	int opt;
	/* No leading '+': let GNU getopt permute so --share can appear after
	 * positional <image> args (which matches the documented usage). */
	while ((opt = getopt_long(argc, argv, "c:dhp:P:", long_opts, NULL)) !=
	       -1) {
		switch (opt) {
		case 1000: /* --share */
			if (n_share_specs >= ANYFS_MAX_SHARES) {
				fprintf(
				    stderr,
				    "error: too many --share flags (max %d)\n",
				    ANYFS_MAX_SHARES);
				return 1;
			}
			share_specs[n_share_specs++] = optarg;
			break;
		case 1001: /* --mem-mb */
			mem_mb = atoi(optarg);
			if (mem_mb <= 0) {
				fprintf(
				    stderr,
				    "error: --mem-mb must be > 0 (got '%s')\n",
				    optarg);
				return 1;
			}
			break;
		case 1002: /* --max-read */
			limits.max_read = atoi(optarg);
			if (limits.max_read < 4096) {
				fprintf(stderr,
					"error: --max-read must be >= 4096 "
					"(got '%s')\n",
					optarg);
				return 1;
			}
			break;
		case 1003: /* --max-conn */
			limits.max_conn = atoi(optarg);
			if (limits.max_conn <= 0) {
				fprintf(stderr,
					"error: --max-conn must be > 0 (got "
					"'%s')\n",
					optarg);
				return 1;
			}
			break;
		case 1004: /* --max-credits */
			limits.max_credits = atoi(optarg);
			if (limits.max_credits <= 0) {
				fprintf(stderr,
					"error: --max-credits must be > 0 (got "
					"'%s')\n",
					optarg);
				return 1;
			}
			break;
		case 1005: /* --busy-spin */
			host_proxy_set_busy_spin(1);
			break;
		case 1006: /* --no-fast-sync */
			fast_sync = 0;
			break;
		case 'c':
			config_file = optarg;
			fprintf(
			    stderr,
			    "warning: -c is deprecated. --share is preferred; "
			    "-c FILE will take precedence over built-in "
			    "config.\n");
			break;
		case 'd':
			log_level = PR_DEBUG;
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
		default:
			usage(stderr, argv[0]);
			return 1;
		}
	}

	/* ── Collect positional disk images ──────────────────────────────── */
	for (; optind < argc; optind++) {
		const char* arg = argv[optind];
		if (n_images >= ANYFS_MAX_DISKS) {
			fprintf(stderr,
				"error: too many disk images (max %d)\n",
				ANYFS_MAX_DISKS);
			return 1;
		}
		disk_images[n_images++] = arg;
	}

	if (n_images == 0) {
		fprintf(stderr, "error: at least one disk image is required\n");
		usage(stderr, argv[0]);
		return 1;
	}

	/* ── Back-compat: -p N → implicit --share ───────────────────────────
	 */
	/* Build a synthetic spec string on the stack for -p N. */
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
		if (n_share_specs < ANYFS_MAX_SHARES)
			share_specs[n_share_specs++] = legacy_spec;
	}

	/* ── Default share if none specified ─────────────────────────────── */
	/* If no --share and no -p, emit a helpful error. */
	if (n_share_specs == 0) {
		fprintf(
		    stderr,
		    "error: no shares specified. "
		    "Use '--share [name=]disk<N>/p<M>' (e.g. --share disk0/p1) "
		    "to expose a partition.\n"
		    "Run 'anyfs-lspart %s' to see partitions in this image.\n"
		    "Run '%s -h' for help.\n",
		    disk_images[0], argv[0]);
		return 1;
	}

	/* ── Signals / stdio ────────────────────────────────────────────────
	 */
	setbuf(stdout, NULL);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	pr_logger_init(PR_LOGGER_STDIO);
	set_log_level(log_level);

	/* ── 1. Boot LKL kernel ─────────────────────────────────────────────
	 */
	if (fast_sync)
		lkl_fastsync_install();
	AnyfsKernelOpts kern_opts = {
	    .mem_mb = mem_mb,
	    .loglevel = (log_level >= PR_DEBUG) ? 8 : 4,
	};
	int ret = anyfs_kernel_init(&kern_opts);
	if (ret) {
		pr_err("Failed to start kernel\n");
		return 1;
	}
	pr_info("LKL kernel started (ksmbd built-in)\n");

	/*
	 * No virtio-net / slirp: the data path is host TCP -> host_proxy
	 * threads -> lkl_sys_read/write -> LKL TCP on lo. We just need lo
	 * up so ksmbd's netdev notifier creates the listener socket bound
	 * to it. (lo is auto-up after boot, but the call is idempotent and
	 * documents intent.)
	 */
	lkl_if_up(1); /* loopback is always ifindex 1 in LKL */

	if (log_level >= PR_DEBUG) {
		long dfd = lkl_sys_open("/sys/class/ksmbd-control/debug",
					LKL_O_WRONLY, 0);
		pr_debug("ksmbd-control/debug open(WRONLY): %ld\n", dfd);
		if (dfd >= 0) {
			long w = lkl_sys_write(dfd, "all\n", 4);
			pr_debug("ksmbd-control/debug write: %ld\n", w);
			lkl_sys_close(dfd);
		}
		long rfd = lkl_sys_open("/sys/class/ksmbd-control/debug",
					LKL_O_RDONLY, 0);
		if (rfd >= 0) {
			char rb[128] = {0};
			long n = lkl_sys_read(rfd, rb, sizeof(rb) - 1);
			pr_debug(
			    "ksmbd-control/debug readback (%ld bytes): %s\n", n,
			    rb);
			lkl_sys_close(rfd);
		}
	}

	/* ── 4. Open disk images ────────────────────────────────────────────
	 */
	AnyfsDisk* disks[ANYFS_MAX_DISKS] = {NULL};

	if (anyfs_sesh_open_disks(disks, disk_images, n_images,
				  ANYFS_DISK_READONLY) < 0)
		goto halt;

	/* ── 5. Resolve --share specs to LKL paths ───────────────────────── */
	ShareInfo shares[ANYFS_MAX_SHARES];
	int n_shares = 0;

	for (int si = 0; si < n_share_specs; si++) {
		ShareInfo* sh = &shares[n_shares];
		if (anyfs_share_resolve(share_specs[si], disks, n_images, 0,
					sh->name, sizeof(sh->name),
					sh->lkl_path, sizeof(sh->lkl_path)) < 0)
			goto halt;
		pr_info("Share [%s] -> %s\n", sh->name, sh->lkl_path);
		n_shares++;
	}

	if (n_shares == 0) {
		pr_err("No shares could be mounted.\n");
		goto halt;
	}

	/* ── 6. Set up ksmbd-tools configuration ────────────────────────────
	 */
	setup_global_conf(&limits);

	/* Both branches go through the same in-memory entry point. -c FILE
	 * supplies extra sections from a host-side smb.conf — we parse it
	 * ourselves and replay through cp_parse_external_smbconf_group rather
	 * than letting ksmbd-tools mmap a file (which doesn't survive CRLF
	 * under wine). */
	if (setup_ksmbd_config(shares, n_shares, config_file) < 0)
		goto halt;

	if (!(ksmbd_health_status & KSMBD_HEALTH_RUNNING)) {
		pr_err("ksmbd IPC init failed\n");
		goto cleanup;
	}

	/*
	 * ksmbd is now listening on lo:SMB_PORT inside LKL. Start the host
	 * TCP proxy that bridges host:host_port to it.
	 */
	if (host_proxy_start(host_port, SMB_PORT) < 0) {
		pr_err("host_proxy_start failed\n");
		goto cleanup;
	}

	pr_info("SMB server ready at localhost:%d\n", host_port);
	for (int i = 0; i < n_shares; i++)
		pr_info(
		    "  smbclient //localhost/%s -U guest%%guest --port=%d\n",
		    shares[i].name, host_port);

	/* ── 7. Event loop ──────────────────────────────────────────────────
	 */
	while (running) {
		ret = ipc_process_event();
		if (ret < 0)
			break;
	}

	pr_info("Shutting down...\n");
	host_proxy_stop();

cleanup:
	ipc_destroy();
	wp_destroy();
	spnego_destroy();
	sm_destroy();
	shm_destroy();
	usm_destroy();

halt:
	/* Close all disk sessions (atexit handler inside anyfs_disk_close
	 * will unmount any LKL-pinned mounts). */
	for (int i = 0; i < n_images; i++) {
		if (disks[i])
			anyfs_disk_close(disks[i]);
	}
	anyfs_kernel_halt();
	pr_info("Done\n");
	return 0;
}
