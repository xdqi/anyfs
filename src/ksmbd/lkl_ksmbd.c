// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lkl_ksmbd.c - LKL-based SMB3 server using ksmbd + ksmbd-tools
 *
 * Boots a Linux Kernel Library instance with the ksmbd module,
 * sets up networking via libslirp (user-mode, no root needed),
 * mounts one or more disk images, and runs the ksmbd-tools IPC daemon
 * to serve SMB3 shares.
 *
 * Build: meson compile -C build
 * Run:   ./anyfs-ksmbd [options] <image>[?<query>] [<image>...] --share
 * name=path ... Test:  smbclient //localhost/esp -U guest%guest --port=4455
 *
 * Examples:
 *   anyfs-ksmbd disk.img --share data=p1
 *   anyfs-ksmbd boot.img data.qcow2 --share esp=disk0/p1 --share home=disk1/p1
 *   anyfs-ksmbd disk.img -P 4450          (back-compat: auto share p0
 * whole-disk) anyfs-ksmbd disk.img -p 2             (deprecated: equivalent to
 * --share p2)
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/path_dsl.h"
#include "../core/share_spec.h"
#include "anyfs.h"
#include "anyfs_disk.h"
#include <lkl.h>

/* Forward declarations for the slirp netdev backend (defined in
 * virtio_net_slirp.c, compiled alongside the binary). */
struct lkl_netdev* lkl_netdev_slirp_create(void);
int lkl_netdev_slirp_add_hostfwd(struct lkl_netdev* nd, int is_udp,
				 const char* host_addr, int host_port,
				 const char* guest_addr, int guest_port);

#include <config_parser.h>
#include <ipc.h>
#include <linux/ksmbd_server.h>
#include <management/session.h>
#include <management/share.h>
#include <management/spnego.h>
#include <management/tree_conn.h>
#include <management/user.h>
#include <tools.h>
#include <worker.h>

/* ── Compile-time limits ─────────────────────────────────────────────── */
#define MAX_DISKS 16
#define MAX_SHARES 32

/* ── Network defaults ─────────────────────────────────────────────────── */
#define GUEST_IP "10.0.2.15"
#define GUEST_GW "10.0.2.2"
#define GUEST_NMLEN 24
#define SMB_PORT 445
#define HOST_FWD_PORT 4455
#define GUEST_USER "guest"

/* ── Share descriptor ─────────────────────────────────────────────────── */
typedef struct {
	char name[64];	   /* SMB share name (section header in smb.conf) */
	char lkl_path[64]; /* absolute LKL path returned by anyfs_disk_enter */
} ShareInfo;

static volatile int running = 1;

static void sigint_handler(int sig)
{
	(void)sig;
	running = 0;
}

static unsigned int ip_str_to_int(const char* s)
{
	unsigned int a, b, c, d;
	sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d);
	return (a) | (b << 8) | (c << 16) | (d << 24);
}

/* ── Global config setup ─────────────────────────────────────────────── */
static void setup_global_conf(void)
{
	memset(&global_conf, 0, sizeof(global_conf));

	global_conf.tcp_port = SMB_PORT;
	global_conf.ipc_timeout = 30;
	global_conf.deadtime = 0;
	global_conf.file_max = 10000;
	global_conf.smb2_max_read = 65536;
	global_conf.smb2_max_write = 65536;
	global_conf.smb2_max_trans = 65536;
	global_conf.smb2_max_credits = 8192;
	global_conf.smbd_max_io_size = 65536;
	global_conf.max_connections = 128;
	global_conf.share_fake_fscaps = 0;
	global_conf.server_signing = 0;

	global_conf.server_min_protocol = g_strdup("SMB2_10");
	global_conf.server_max_protocol = g_strdup("SMB3_11");
	global_conf.netbios_name = g_strdup("LKLSMB");
	global_conf.server_string = g_strdup("LKL ksmbd Server");
	global_conf.work_group = g_strdup("WORKGROUP");
	global_conf.guest_account = g_strdup(GUEST_USER);

	global_conf.map_to_guest = KSMBD_CONF_MAP_TO_GUEST_BAD_USER;
}

/* ── Config file generation ──────────────────────────────────────────── */

/*
 * Write a [global] + one [section] per share and load it via ksmbd-tools.
 * Returns 0 on success.
 */
static int setup_ksmbd_config(const ShareInfo* shares, int n_shares)
{
	const char* conf_path = "/tmp/lkl_ksmbd.conf";
	const char* pwd_path = "/tmp/lkl_ksmbdpwd_nonexistent.db";

	FILE* fp = fopen(conf_path, "w");
	if (!fp) {
		perror("fopen conf");
		return -1;
	}

	fprintf(fp,
		"[global]\n"
		"\tserver string = LKL ksmbd Server\n"
		"\tnetbios name = LKLSMB\n"
		"\tworkgroup = WORKGROUP\n"
		"\tserver min protocol = SMB2_10\n"
		"\tserver max protocol = SMB3_11\n"
		"\tmap to guest = bad user\n"
		"\tguest account = %s\n",
		GUEST_USER);

	for (int i = 0; i < n_shares; i++) {
		fprintf(fp,
			"\n"
			"[%s]\n"
			"\tpath = %s\n"
			"\tguest ok = yes\n"
			"\tread only = yes\n"
			"\tbrowseable = yes\n"
			"\tforce user = root\n",
			shares[i].name, shares[i].lkl_path);
	}
	fclose(fp);

	int ret = load_config((char*)pwd_path, (char*)conf_path);
	if (ret) {
		pr_err("load_config failed: %d\n", ret);
		return -1;
	}
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
	    "                     'path' uses the canonical path DSL:\n"
	    "                       p1              single-disk: partition 1 "
	    "(auto disk0/p1)\n"
	    "                       disk0/p1        explicit disk + partition\n"
	    "                       disk1/p2        partition 2 of the second "
	    "image\n"
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
	    "  -h, --help         Show this help.\n"
	    "\n"
	    "Examples:\n"
	    "  %s disk.img --share data=p1\n"
	    "  %s disk.img -P 4450\n"
	    "  %s boot.img data.qcow2 --share esp=disk0/p1 --share "
	    "home=disk1/p1\n"
	    "  Then: smbclient //localhost/<name> -U guest%%guest --port=%d\n",
	    prog, HOST_FWD_PORT, prog, prog, prog, HOST_FWD_PORT);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char** argv)
{
	/* ── Argument storage ───────────────────────────────────────────────
	 */
	const char* disk_images[MAX_DISKS];
	int n_images = 0;
	/* raw share specs accumulated during getopt_long */
	char* share_specs[MAX_SHARES];
	int n_share_specs = 0;

	const char* config_file = NULL;
	int log_level = PR_INFO;
	int host_port = HOST_FWD_PORT;
	int legacy_part = -1; /* -p N, -1 = not set */

	/* ── Option parsing ─────────────────────────────────────────────────
	 */
	static const struct option long_opts[] = {
	    {"share", required_argument, NULL, 1000},
	    {"help", no_argument, NULL, 'h'},
	    {NULL, 0, NULL, 0}};

	int opt;
	/* No leading '+': let GNU getopt permute so --share can appear after
	 * positional <image> args (which matches the documented usage). */
	while ((opt = getopt_long(argc, argv, "c:dhp:P:", long_opts, NULL)) !=
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
		if (n_images >= MAX_DISKS) {
			fprintf(stderr,
				"error: too many disk images (max %d)\n",
				MAX_DISKS);
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
		if (n_share_specs < MAX_SHARES)
			share_specs[n_share_specs++] = legacy_spec;
	}

	/* ── Default share if none specified ─────────────────────────────── */
	/* If no --share and no -p, emit a helpful error. */
	if (n_share_specs == 0) {
		fprintf(stderr,
			"error: no shares specified. "
			"Use '--share [name=]p<N>' (e.g. --share p1) to expose "
			"a partition.\n"
			"Run '%s -h' for help.\n",
			argv[0]);
		return 1;
	}

	/* ── Signals / stdio ────────────────────────────────────────────────
	 */
	setbuf(stdout, NULL);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	pr_logger_init(PR_LOGGER_STDIO);
	set_log_level(log_level);

	/* ── 1. Create slirp network device ──────────────────────────────── */
	struct lkl_netdev* nd = lkl_netdev_slirp_create();
	if (!nd) {
		pr_err("Failed to create slirp netdev\n");
		return 1;
	}

	int ret = lkl_netdev_slirp_add_hostfwd(nd, 0, "0.0.0.0", host_port,
					       GUEST_IP, SMB_PORT);
	if (ret < 0) {
		pr_err("Failed to add port forward\n");
		return 1;
	}
	pr_info("Port forward: host *:%d -> guest %s:%d\n", host_port, GUEST_IP,
		SMB_PORT);

	/* ── 2. Boot LKL kernel ─────────────────────────────────────────────
	 */
	AnyfsKernelOpts kern_opts = {.mem_mb = 64, .loglevel = 4};
	ret = anyfs_kernel_init(&kern_opts);
	if (ret) {
		pr_err("Failed to start kernel\n");
		return 1;
	}
	pr_info("LKL kernel started (ksmbd built-in)\n");

	/* ── 3. Configure network ───────────────────────────────────────────
	 */
	struct lkl_netdev_args nd_args;
	__lkl__u8 mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
	memset(&nd_args, 0, sizeof(nd_args));
	nd_args.mac = mac;

	int nd_id = lkl_netdev_add(nd, &nd_args);
	if (nd_id < 0) {
		pr_err("Failed to add netdev: %s\n", lkl_strerror(nd_id));
		goto halt;
	}

	int ifindex = lkl_netdev_get_ifindex(nd_id);
	lkl_if_up(ifindex);
	lkl_if_set_ipv4(ifindex, ip_str_to_int(GUEST_IP), GUEST_NMLEN);
	lkl_set_ipv4_gateway(ip_str_to_int(GUEST_GW));
	pr_info("Network: %s/%d gw %s\n", GUEST_IP, GUEST_NMLEN, GUEST_GW);

	/* ── 4. Open disk images ────────────────────────────────────────────
	 */
	AnyfsDisk* disks[MAX_DISKS] = {NULL};
	int n_open = 0;

	for (int i = 0; i < n_images; i++) {
		/* Strip optional ?<query> suffix from the image path (v1:
		 * ignored). */
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

		if (anyfs_disk_open(img, ANYFS_DISK_READONLY, &disks[i]) < 0 ||
		    !disks[i]) {
			pr_err("Failed to open disk image '%s'\n",
			       disk_images[i]);
			goto halt;
		}
		n_open++;
		pr_info("Opened disk%d: %s (id=%d)\n", i, img,
			anyfs_disk_id(disks[i]));
	}

	/* ── 5. Resolve --share specs to LKL paths ───────────────────────── */
	ShareInfo shares[MAX_SHARES];
	int n_shares = 0;

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

		/* Validate disk index in range */
		if (disk_idx >= n_images) {
			fprintf(stderr,
				"error: disk%d not registered (only "
				"disk0..disk%d available).\n",
				disk_idx, n_images - 1);
			anyfs_path_dsl_free(&ap);
			goto halt;
		}

		/* Must have at least one path component */
		if (ap.n_comp == 0) {
			fprintf(stderr,
				"error: --share path '%s' has no partition "
				"component.\n",
				path_arg);
			anyfs_path_dsl_free(&ap);
			goto halt;
		}

		/* Validate and warn about credentials */
		anyfs_share_warn_literal_key(&ap,
					     name_arg ? name_arg : path_arg);

		/* Walk every component (v2): containers along the way are
		 * entered automatically; the final segment must be FS. */
		char lkl_path[64];
		ret = anyfs_disk_enter_path(disks[disk_idx], ap.comp, ap.n_comp,
					    0, lkl_path);
		if (ret < 0) {
			/* Get the reason from the slot that failed — for
			 * top-level single-component shares this matches the v1
			 * behaviour. */
			const char* reason = anyfs_disk_fail_reason(
			    disks[disk_idx], ap.comp[0].p);
			fprintf(stderr,
				"error: cannot enter %s: %s\n"
				"Containers (LVM_PV, LUKS, nested partition "
				"table) require\n"
				"either a credential (`?keyref=`/`keyfile=`) "
				"or v3 support.\n"
				"Use 'anyfs-lspart' to discover the canonical "
				"leaf path.\n",
				path_arg, reason ? reason : lkl_strerror(ret));
			anyfs_path_dsl_free(&ap);
			goto halt;
		}

		/* Build a canonical path string for share-name derivation. */
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

		/* Derive the share name if not given */
		ShareInfo* sh = &shares[n_shares];
		if (name_arg && *name_arg) {
			strncpy(sh->name, name_arg, sizeof(sh->name) - 1);
			sh->name[sizeof(sh->name) - 1] = '\0';
		} else {
			anyfs_share_auto_name(canonical, sh->name,
					      sizeof(sh->name));
		}
		strncpy(sh->lkl_path, lkl_path, sizeof(sh->lkl_path) - 1);
		sh->lkl_path[sizeof(sh->lkl_path) - 1] = '\0';

		pr_info("Share [%s] -> %s (%s)\n", sh->name, sh->lkl_path,
			canonical);
		anyfs_path_dsl_free(&ap);
		n_shares++;
	}

	if (n_shares == 0) {
		pr_err("No shares could be mounted.\n");
		goto halt;
	}

	/* ── 6. Set up ksmbd-tools configuration ────────────────────────────
	 */
	setup_global_conf();
	extern tool_main_fn* tool_main;
	tool_main = mountd_main;

	if (config_file) {
		/* User-supplied config file takes precedence */
		if (load_config("/dev/null", (char*)config_file)) {
			pr_err("load_config failed\n");
			goto halt;
		}
	} else {
		if (setup_ksmbd_config(shares, n_shares) < 0)
			goto halt;
	}

	if (!(ksmbd_health_status & KSMBD_HEALTH_RUNNING)) {
		pr_err("ksmbd IPC init failed\n");
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
	for (int i = 0; i < n_open; i++) {
		if (disks[i])
			anyfs_disk_close(disks[i]);
	}
	anyfs_kernel_halt();
	pr_info("Done\n");
	return 0;
}
