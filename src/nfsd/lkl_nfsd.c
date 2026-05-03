// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lkl_nfsd.c - LKL-based NFSv4 server
 *
 * Boots a Linux Kernel Library instance with the NFS server (nfsd),
 * sets up networking via libslirp, mounts a disk image, and starts
 * nfsd serving NFSv4 only on port 2049 (forwarded to a host port).
 *
 * Includes a mini "mountd" that handles sunrpc cache upcalls:
 *   - auth.unix.ip:  maps client IP -> auth domain "unix"
 *   - auth.unix.gid: maps UID -> supplementary GIDs
 *   - nfsd.fh:       maps (domain, fsid) -> export path
 *   - nfsd.export:   maps (domain, path) -> export flags
 *
 * Build: meson compile -C builddir-ksmbd
 * Run:   ./lkl_nfsd <disk.img>
 * Test:  mount -t nfs4 localhost:/ /mnt -o port=20049
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "anyfs.h"
#include <lkl.h>

/* Default configuration */
#define GUEST_IP "10.0.2.15"
#define GUEST_GW "10.0.2.2"
#define GUEST_NMLEN 24
#define NFS_PORT 2049
#define HOST_FWD_PORT 20049
#define MOUNT_NAME "nfs"

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
 *   auth.unix.ip:  query "class ip"       -> response "class ip expiry domain"
 *   auth.unix.gid: query "uid"            -> response "uid expiry 0" (no
 * supplementary gids) nfsd.fh:       query "domain type fsid" -> response
 * "domain type fsid expiry path" nfsd.export:   query "domain path"    ->
 * response "domain path expiry flags anonuid anongid fsid"
 */

/* Export configuration - set by main before starting handler */
static char nfs_export_path[256];
static int nfs_read_only = 1;

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

	/* Map any IP to the "unix" auth domain */
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

	/* uid expiry Ngids - respond with 0 supplementary groups */
	snprintf(resp, sizeof(resp), "%s %ld 0\n", uid_str, (long)expiry);
	printf("[mountd] unix_gid: uid %s -> 0 groups\n", uid_str);
	lkl_sys_write(fd, resp, strlen(resp));
}

/* Handle nfsd.fh upcall: "domain fsidtype \xfsid\n" -> respond with path */
static void handle_expkey(int fd, char* query)
{
	char domain[64], fsidtype_str[16], fsid_hex[64], resp[512];
	char* p = query;
	int fsidtype, fsid_len;
	time_t expiry = time(NULL) + 3600 * 24 * 365;

	if (qword_get_user(&p, domain, sizeof(domain)) <= 0)
		return;
	if (qword_get_user(&p, fsidtype_str, sizeof(fsidtype_str)) <= 0)
		return;
	fsidtype = atoi(fsidtype_str);
	fsid_len = qword_get_user(&p, fsid_hex, sizeof(fsid_hex));
	if (fsid_len <= 0)
		return;

	/* Only handle FSID_NUM (type 1) with fsid=0 (our root export) */
	if (fsidtype == 1 && fsid_len == 4 && fsid_hex[0] == 0 &&
	    fsid_hex[1] == 0 && fsid_hex[2] == 0 && fsid_hex[3] == 0) {
		snprintf(resp, sizeof(resp), "%s 1 \\x00000000 %ld %s\n",
			 domain, (long)expiry, nfs_export_path);
		printf("[mountd] expkey: %s fsid=0 -> %s\n", domain,
		       nfs_export_path);
		lkl_sys_write(fd, resp, strlen(resp));
	} else {
		/* Negative response: domain fsidtype fsid expiry (no path) */
		/* Re-encode fsid as hex */
		char hex[128];
		int i, off = 0;
		off += snprintf(hex + off, sizeof(hex) - off, "\\x");
		for (i = 0; i < fsid_len && off < (int)sizeof(hex) - 2; i++)
			off += snprintf(hex + off, sizeof(hex) - off, "%02x",
					(unsigned char)fsid_hex[i]);
		snprintf(resp, sizeof(resp), "%s %d %s %ld\n", domain, fsidtype,
			 hex, (long)expiry);
		printf("[mountd] expkey: %s fsid_type=%d -> NEGATIVE\n", domain,
		       fsidtype);
		lkl_sys_write(fd, resp, strlen(resp));
	}
}

/* Handle nfsd.export upcall: "domain path\n" -> respond with export flags */
static void handle_export(int fd, char* query)
{
	char domain[64], path[256], resp[512];
	char* p = query;
	time_t expiry = time(NULL) + 3600 * 24 * 365;

	if (qword_get_user(&p, domain, sizeof(domain)) <= 0)
		return;
	if (qword_get_user(&p, path, sizeof(path)) <= 0)
		return;

	/* Check if this is our exported path (or a parent of it) */
	if (strcmp(path, nfs_export_path) == 0) {
		unsigned int flags = NFSEXP_INSECURE | NFSEXP_NOSUBTREECHECK |
				     NFSEXP_FSID | NFSEXP_ALLSQUASH;
		if (nfs_read_only)
			flags |= NFSEXP_READONLY;
		/* anonuid=0 anongid=0: ALLSQUASH maps all users to root */
		snprintf(resp, sizeof(resp), "%s %s %ld %u 0 0 0\n", domain,
			 path, (long)expiry, flags);
		printf("[mountd] export: %s %s -> flags=%#x\n", domain, path,
		       flags);
		lkl_sys_write(fd, resp, strlen(resp));
	} else {
		/* Negative: domain path expiry (no flags -> negative entry) */
		snprintf(resp, sizeof(resp), "%s %s %ld\n", domain, path,
			 (long)expiry);
		printf("[mountd] export: %s %s -> NEGATIVE\n", domain, path);
		lkl_sys_write(fd, resp, strlen(resp));
	}
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

	/* Open all channel files for read+write */
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
		/* Poll channels for upcall queries */
		struct lkl_pollfd pfds[NUM_CHANNELS];
		int nfds = 0;
		int fd_map[NUM_CHANNELS]; /* maps pfd index -> channel index */

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

		int ret = lkl_sys_poll(pfds, nfds, 200 /* ms */);
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

			/* The query may contain multiple lines */
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

	/* Mount the nfsd control filesystem */
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

	/* Disable NFSv3, keep only NFSv4 */
	ret = lkl_write_file("/proc/fs/nfsd/versions", "-3 +4\n");
	if (ret < 0) {
		fprintf(stderr, "Warning: could not set versions\n");
	}

	/* Start nfsd thread(s) - this creates the service and listeners */
	ret = lkl_write_file("/proc/fs/nfsd/threads", "1\n");
	if (ret < 0) {
		fprintf(stderr, "Failed to start nfsd threads: %s\n",
			lkl_strerror(ret));
		return ret;
	}
	printf("nfsd thread started\n");

	/* End NFSv4 grace period immediately (no clients to reclaim) */
	lkl_write_file("/proc/fs/nfsd/v4_end_grace", "Y\n");

	return 0;
}

int main(int argc, char** argv)
{
	const char* disk_image = NULL;
	unsigned int partition = 0;
	int host_port = HOST_FWD_PORT;
	int read_only = 1;
	struct lkl_netdev* nd;
	struct lkl_netdev_args nd_args;
	__lkl__u8 mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};
	int nd_id, ifindex, ret;
	int opt;

	while ((opt = getopt(argc, argv, "p:P:w")) != -1) {
		switch (opt) {
		case 'p':
			partition = atoi(optarg);
			break;
		case 'P':
			host_port = atoi(optarg);
			break;
		case 'w':
			read_only = 0;
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-p partition] [-P port] [-w] "
				"<disk-image>\n",
				argv[0]);
			fprintf(stderr,
				"  -p N   use partition N (0=whole disk)\n");
			fprintf(stderr,
				"  -P N   host port for NFS (default: %d)\n",
				HOST_FWD_PORT);
			fprintf(stderr, "  -w     read-write export (default: "
					"read-only)\n");
			return 1;
		}
	}

	if (optind >= argc) {
		fprintf(
		    stderr,
		    "Usage: %s [-p partition] [-P port] [-w] <disk-image>\n",
		    argv[0]);
		return 1;
	}
	disk_image = argv[optind];

	setbuf(stdout, NULL);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	/* 1. Create slirp network device */
	nd = lkl_netdev_slirp_create();
	if (!nd) {
		fprintf(stderr, "Failed to create slirp netdev\n");
		return 1;
	}

	ret = lkl_netdev_slirp_add_hostfwd(nd, 0, "0.0.0.0", host_port,
					   GUEST_IP, NFS_PORT);
	if (ret < 0) {
		fprintf(stderr, "Failed to add port forward\n");
		return 1;
	}
	printf("Port forward: host *:%d -> guest %s:%d\n", host_port, GUEST_IP,
	       NFS_PORT);

	/* 2. Boot LKL kernel */
	AnyfsKernelOpts kern_opts = {.mem_mb = 64, .loglevel = 4};
	ret = anyfs_kernel_init(&kern_opts);
	if (ret) {
		fprintf(stderr, "Failed to start kernel\n");
		return 1;
	}
	printf("LKL kernel started (nfsd built-in)\n");

	/* 3. Configure network */
	memset(&nd_args, 0, sizeof(nd_args));
	nd_args.mac = mac;
	nd_id = lkl_netdev_add(nd, &nd_args);
	if (nd_id < 0) {
		fprintf(stderr, "Failed to add netdev: %s\n",
			lkl_strerror(nd_id));
		goto halt;
	}

	ifindex = lkl_netdev_get_ifindex(nd_id);
	lkl_if_up(ifindex);
	lkl_if_set_ipv4(ifindex, ip_str_to_int(GUEST_IP), GUEST_NMLEN);
	lkl_set_ipv4_gateway(ip_str_to_int(GUEST_GW));
	printf("Network: %s/%d gw %s\n", GUEST_IP, GUEST_NMLEN, GUEST_GW);

	/* Bring up loopback (needed for rpcbind local connection) */
	lkl_if_up(1); /* lo is always ifindex 1 */

	/* 4. Mount disk image */
	{
		uint32_t dflags = read_only ? ANYFS_DISK_READONLY : 0;
		int disk_id = anyfs_disk_add(disk_image, dflags);
		if (disk_id < 0) {
			fprintf(stderr, "anyfs_disk_add failed\n");
			goto halt;
		}

		uint32_t mflags = read_only ? ANYFS_MOUNT_RDONLY : 0;
		AnyfsMount mnt_info;
		ret = anyfs_mount(disk_id, partition, NULL, MOUNT_NAME, mflags,
				  &mnt_info);
		if (ret < 0) {
			fprintf(stderr, "anyfs_mount failed\n");
			goto halt;
		}
		printf("Mounted %s (%s) at %s\n", disk_image, mnt_info.fstype,
		       mnt_info.mount_point);
	}

	/* 5. Set up NFS export path for cache handler */
	snprintf(nfs_export_path, sizeof(nfs_export_path), "/lklmnt/%s",
		 MOUNT_NAME);
	nfs_read_only = read_only;
	printf("Export path: %s\n", nfs_export_path);

	/* 6. Start nfsd */
	if (start_nfsd() < 0)
		goto halt;

	/* 7. Start cache upcall handler thread (mini mountd) */
	pthread_t cache_tid;
	if (pthread_create(&cache_tid, NULL, cache_handler_thread, NULL) != 0) {
		perror("pthread_create");
		goto halt;
	}

	printf("\n=== NFSv4 server ready ===\n");
	printf("Mount with: mount -t nfs4 localhost:/ /mnt -o port=%d,vers=4\n",
	       host_port);
	printf("Press Ctrl+C to stop.\n\n");

	/* 8. Serve until interrupted */
	while (running) {
		usleep(100000); /* 100ms poll */
	}

	printf("\nShutting down...\n");
	pthread_join(cache_tid, NULL);

halt:
	anyfs_umount(MOUNT_NAME);
	anyfs_kernel_halt();
	printf("Done\n");
	return 0;
}
