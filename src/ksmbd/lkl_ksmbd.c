// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lkl_ksmbd.c - LKL-based SMB3 server using ksmbd + ksmbd-tools
 *
 * Boots a Linux Kernel Library instance with the ksmbd module,
 * sets up networking via libslirp (user-mode, no root needed),
 * mounts a disk image, and runs the ksmbd-tools IPC daemon
 * to serve SMB3 shares.
 *
 * Build: make -f Makefile.ksmbd
 * Run:   ./lkl_ksmbd <disk.img> [config-file]
 * Test:  smbclient //localhost/share -U guest%guest --port=4455
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "anyfs.h"
#include <lkl.h>

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

/* Default configuration */
#define GUEST_IP "10.0.2.15"
#define GUEST_GW "10.0.2.2"
#define GUEST_NMLEN 24
#define SMB_PORT 445
#define HOST_FWD_PORT 4455
#define SHARE_PATH "/lklmnt/share"
#define SHARE_NAME "share"
#define GUEST_USER "guest"

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

/* Set up default global_conf for ksmbd */
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

	/* Map bad-user to guest for anonymous access */
	global_conf.map_to_guest = KSMBD_CONF_MAP_TO_GUEST_BAD_USER;
}

/* Create config files on host and load them */
static int setup_ksmbd_config(void)
{
	const char* conf_path = "/tmp/lkl_ksmbd.conf";
	/* Use non-existent pwddb - mountd mode treats ENOENT as "no users" */
	const char* pwd_path = "/tmp/lkl_ksmbdpwd_nonexistent.db";

	/* Create config file on host */
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
		"\tguest account = %s\n"
		"\n"
		"[%s]\n"
		"\tpath = %s\n"
		"\tguest ok = yes\n"
		"\tread only = yes\n"
		"\tbrowseable = yes\n"
		"\tforce user = root\n",
		GUEST_USER, SHARE_NAME, SHARE_PATH);
	fclose(fp);

	/* Use ksmbd-tools' load_config to parse and register everything */
	int ret = load_config((char*)pwd_path, (char*)conf_path);
	if (ret) {
		pr_err("load_config failed: %d\n", ret);
		return -1;
	}

	return 0;
}

int main(int argc, char** argv)
{
	const char* disk_image = NULL;
	const char* config_file = NULL;
	int log_level = PR_INFO;
	unsigned int partition = 0;
	int host_port = HOST_FWD_PORT;
	struct lkl_netdev* nd;
	struct lkl_netdev_args nd_args;
	__lkl__u8 mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
	int nd_id, ifindex, ret;
	int opt;

	while ((opt = getopt(argc, argv, "c:dp:P:")) != -1) {
		switch (opt) {
		case 'c':
			config_file = optarg;
			break;
		case 'd':
			log_level = PR_DEBUG;
			break;
		case 'p':
			partition = atoi(optarg);
			break;
		case 'P':
			host_port = atoi(optarg);
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-d] [-c ksmbd.conf] [-p partition] "
				"[-P port] <disk-image>\n",
				argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr,
			"Usage: %s [-d] [-c ksmbd.conf] [-p partition] [-P "
			"port] <disk-image>\n",
			argv[0]);
		return 1;
	}
	disk_image = argv[optind];

	setbuf(stdout, NULL);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	pr_logger_init(PR_LOGGER_STDIO);
	set_log_level(log_level);

	/* 1. Create slirp network device */
	nd = lkl_netdev_slirp_create();
	if (!nd) {
		pr_err("Failed to create slirp netdev\n");
		return 1;
	}

	ret = lkl_netdev_slirp_add_hostfwd(nd, 0, "0.0.0.0", host_port,
					   GUEST_IP, SMB_PORT);
	if (ret < 0) {
		pr_err("Failed to add port forward\n");
		return 1;
	}
	pr_info("Port forward: host *:%d -> guest %s:%d\n", host_port, GUEST_IP,
		SMB_PORT);

	/* 2. Boot LKL kernel */
	AnyfsKernelOpts kern_opts = {.mem_mb = 64, .loglevel = 4};
	ret = anyfs_kernel_init(&kern_opts);
	if (ret) {
		pr_err("Failed to start kernel\n");
		return 1;
	}
	pr_info("LKL kernel started (ksmbd built-in)\n");

	/* 3. Configure network */
	memset(&nd_args, 0, sizeof(nd_args));
	nd_args.mac = mac;
	nd_id = lkl_netdev_add(nd, &nd_args);
	if (nd_id < 0) {
		pr_err("Failed to add netdev: %s\n", lkl_strerror(nd_id));
		goto halt;
	}

	ifindex = lkl_netdev_get_ifindex(nd_id);
	lkl_if_up(ifindex);
	lkl_if_set_ipv4(ifindex, ip_str_to_int(GUEST_IP), GUEST_NMLEN);
	lkl_set_ipv4_gateway(ip_str_to_int(GUEST_GW));
	pr_info("Network: %s/%d gw %s\n", GUEST_IP, GUEST_NMLEN, GUEST_GW);

	/* 4. Mount disk image */
	{
		int disk_id = anyfs_disk_add(disk_image, 0);
		if (disk_id < 0) {
			pr_err("anyfs_disk_add failed\n");
			goto halt;
		}

		AnyfsMount mnt_info;
		ret = anyfs_mount(disk_id, partition, NULL, "share", 0,
				  &mnt_info);
		if (ret < 0) {
			pr_err("anyfs_mount failed (no supported filesystem "
			       "found)\n");
			goto halt;
		}
		pr_info("Mounted %s (%s) at %s\n", disk_image, mnt_info.fstype,
			mnt_info.mount_point);
	}

	/* 5. Set up ksmbd-tools configuration */
	setup_global_conf();
	/* Set tool_main so TOOL_IS_MOUNTD is true */
	extern tool_main_fn* tool_main;
	tool_main = mountd_main;
	if (config_file) {
		/* Use external config file */
		if (load_config("/dev/null", (char*)config_file)) {
			pr_err("load_config failed\n");
			goto halt;
		}
	} else {
		/* Create config on LKL filesystem and load */
		if (setup_ksmbd_config() < 0)
			goto halt;
	}

	if (!(ksmbd_health_status & KSMBD_HEALTH_RUNNING)) {
		pr_err("ksmbd IPC init failed\n");
		goto cleanup;
	}

	pr_info("SMB server ready at localhost:%d\n", host_port);
	pr_info("Test: smbclient //localhost/%s -U guest%%guest --port=%d\n",
		SHARE_NAME, host_port);

	/* 7. Event loop - handle ksmbd IPC requests */
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
	anyfs_umount(SHARE_NAME);
	anyfs_kernel_halt();
	pr_info("Done\n");
	return 0;
}
