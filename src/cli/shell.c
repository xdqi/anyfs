/*
 * anyfs-shell: interactive filesystem image explorer
 * Uses anyfs.h for kernel/disk management, then LKL syscalls directly.
 */

#include <ctype.h>
#include <errno.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "anyfs.h"
#include <lkl/asm-generic/fcntl.h>
#include <lkl/linux/stat.h>

/* d_type values (same as Linux DT_*) */
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

/* LKL_SEEK_SET if not defined */
#ifndef LKL_SEEK_SET
#define LKL_SEEK_SET 0
#endif

/* Shell state */
static int g_disk_id = -1;
static char g_mount_point[32];
static char g_cwd[4096] = "/";
static char g_image_path[4096];
static char g_fstype[64];
static int g_mounted;

/* Forward declarations */
typedef int (*cmd_fn)(int argc, char** argv);

struct command {
	const char* name;
	const char* usage;
	const char* help;
	cmd_fn fn;
};

static int cmd_open(int argc, char** argv);
static int cmd_mount(int argc, char** argv);
static int cmd_umount(int argc, char** argv);
static int cmd_ls(int argc, char** argv);
static int cmd_ll(int argc, char** argv);
static int cmd_cat(int argc, char** argv);
static int cmd_head(int argc, char** argv);
static int cmd_tail(int argc, char** argv);
static int cmd_stat(int argc, char** argv);
static int cmd_hexdump(int argc, char** argv);
static int cmd_find(int argc, char** argv);
static int cmd_download(int argc, char** argv);
static int cmd_df(int argc, char** argv);
static int cmd_cd(int argc, char** argv);
static int cmd_pwd(int argc, char** argv);
static int cmd_lcd(int argc, char** argv);
static int cmd_help(int argc, char** argv);
static int cmd_quit(int argc, char** argv);

static struct command commands[] = {
    {"open", "open <image> [backend]",
     "Open a disk image (backend: raw, gio, qemu)", cmd_open},
    {"mount", "mount [fstype] [part]",
     "Mount filesystem (auto-detect if no fstype)", cmd_mount},
    {"umount", "umount", "Unmount current filesystem", cmd_umount},
    {"ls", "ls [path]", "List directory contents", cmd_ls},
    {"ll", "ll [path]", "Long listing (type, size, name)", cmd_ll},
    {"cat", "cat <path>", "Print file contents to stdout", cmd_cat},
    {"head", "head [-n N] <path>", "Show first N lines (default 10)", cmd_head},
    {"tail", "tail [-n N] <path>", "Show last N lines (default 10)", cmd_tail},
    {"stat", "stat <path>", "Show file information", cmd_stat},
    {"hexdump", "hexdump <path> [off] [len]", "Hex dump of file", cmd_hexdump},
    {"find", "find [path]", "Recursive file listing", cmd_find},
    {"download", "download <remote> <local>",
     "Download file to local filesystem", cmd_download},
    {"df", "df", "Show filesystem disk space usage", cmd_df},
    {"cd", "cd <path>", "Change guest directory", cmd_cd},
    {"pwd", "pwd", "Print guest working directory", cmd_pwd},
    {"lcd", "lcd <path>", "Change local (host) directory", cmd_lcd},
    {"help", "help [command]", "Show help", cmd_help},
    {"quit", "quit", "Exit the shell", cmd_quit},
    {"exit", "exit", "Exit the shell", cmd_quit},
    {NULL, NULL, NULL, NULL}};

/* --- Utility functions --- */

static void resolve_path(const char* input, char* out, size_t outlen)
{
	if (!input || !input[0]) {
		snprintf(out, outlen, "%s", g_cwd);
		return;
	}
	if (input[0] == '/') {
		snprintf(out, outlen, "%s", input);
	} else {
		if (strcmp(g_cwd, "/") == 0)
			snprintf(out, outlen, "/%s", input);
		else
			snprintf(out, outlen, "%s/%s", g_cwd, input);
	}
	size_t len = strlen(out);
	while (len > 1 && out[len - 1] == '/')
		out[--len] = '\0';
}

/* Build full LKL path: mount_point + guest_path */
static void full_path(const char* guest_path, char* out, size_t outlen)
{
	snprintf(out, outlen, "%s%s%s", g_mount_point,
		 (guest_path[0] == '/') ? "" : "/", guest_path);
}

static const char* human_size(uint64_t size, char* buf, size_t buflen)
{
	if (size < 1024)
		snprintf(buf, buflen, "%llu", (unsigned long long)size);
	else if (size < 1024 * 1024)
		snprintf(buf, buflen, "%.1fK", size / 1024.0);
	else if (size < 1024ULL * 1024 * 1024)
		snprintf(buf, buflen, "%.1fM", size / (1024.0 * 1024));
	else
		snprintf(buf, buflen, "%.1fG", size / (1024.0 * 1024 * 1024));
	return buf;
}

/* --- Command implementations --- */

static int cmd_open(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: open <image> [backend]\n");
		return -1;
	}
	if (g_disk_id >= 0) {
		fprintf(stderr,
			"Image already open. Use 'umount' then close.\n");
		return -1;
	}

	uint32_t flags = ANYFS_DISK_READONLY;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "raw") == 0)
			flags |= ANYFS_BACKEND_RAW;
		else if (strcmp(argv[i], "gio") == 0)
			flags |= ANYFS_BACKEND_GIO;
		else if (strcmp(argv[i], "qemu") == 0)
			flags |= ANYFS_BACKEND_QEMU;
	}

	int id = anyfs_disk_add(argv[1], flags);
	if (id < 0) {
		fprintf(stderr, "Failed to open image: %s\n", argv[1]);
		return -1;
	}
	g_disk_id = id;
	snprintf(g_image_path, sizeof(g_image_path), "%s", argv[1]);
	printf("Opened: %s (disk_id=%d)\n", argv[1], id);
	return 0;
}

static int cmd_mount(int argc, char** argv)
{
	if (g_mounted) {
		fprintf(stderr, "Already mounted. Use 'umount' first.\n");
		return -1;
	}
	if (g_disk_id < 0) {
		fprintf(stderr, "No image open. Use 'open' first.\n");
		return -1;
	}

	uint32_t part = 0;
	const char* fstype = NULL;

	/* mount [fstype] [part] — fstype is optional for auto-detect */
	if (argc >= 2) {
		/* If first arg is numeric, it's a partition number */
		if (argv[1][0] >= '0' && argv[1][0] <= '9')
			part = (uint32_t)atoi(argv[1]);
		else
			fstype = argv[1];
	}
	if (argc >= 3)
		part = (uint32_t)atoi(argv[2]);

	if (fstype) {
		/* Explicit filesystem type */
		AnyfsMount mnt_info;
		int ret = anyfs_mount(g_disk_id, part, fstype, "shell",
				      ANYFS_MOUNT_RDONLY, &mnt_info);
		if (ret) {
			fprintf(stderr, "Mount failed: %d\n", ret);
			return -1;
		}
		strncpy(g_mount_point, mnt_info.mount_point,
			sizeof(g_mount_point) - 1);
		strncpy(g_fstype, mnt_info.fstype, sizeof(g_fstype) - 1);
	} else {
		/* Auto-detect filesystem type */
		AnyfsMount mnt_info;
		int ret = anyfs_mount(g_disk_id, part, NULL, "shell",
				      ANYFS_MOUNT_RDONLY, &mnt_info);
		if (ret) {
			fprintf(
			    stderr,
			    "Mount failed (no supported filesystem found)\n");
			return -1;
		}
		strncpy(g_mount_point, mnt_info.mount_point,
			sizeof(g_mount_point) - 1);
		strncpy(g_fstype, mnt_info.fstype, sizeof(g_fstype) - 1);
	}

	g_mounted = 1;
	strcpy(g_cwd, "/");
	printf("Mounted %s (partition %u) at %s\n", g_fstype, part,
	       g_mount_point);
	return 0;
}

static int cmd_umount(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	/* Unmount using anyfs_umount (mount name is "shell") */
	int ret = anyfs_umount("shell");
	if (ret)
		fprintf(stderr, "Warning: umount returned %ld\n", ret);
	g_mounted = 0;
	g_fstype[0] = '\0';
	g_mount_point[0] = '\0';
	strcpy(g_cwd, "/");
	printf("Unmounted.\n");
	return 0;
}

static int cmd_ls(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(argc >= 2 ? argv[1] : NULL, gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	int err;
	struct lkl_dir* dir = lkl_opendir(fpath, &err);
	if (!dir) {
		fprintf(stderr, "Cannot open directory: %s\n", gpath);
		return -1;
	}

	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir)) != NULL) {
		if (de->d_type == DT_DIR)
			printf("%s/\n", de->d_name);
		else if (de->d_type == DT_LNK)
			printf("%s@\n", de->d_name);
		else
			printf("%s\n", de->d_name);
	}
	lkl_closedir(dir);
	return 0;
}

static int cmd_ll(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(argc >= 2 ? argv[1] : NULL, gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	int err;
	struct lkl_dir* dir = lkl_opendir(fpath, &err);
	if (!dir) {
		fprintf(stderr, "Cannot open directory: %s\n", gpath);
		return -1;
	}

	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir)) != NULL) {
		char type_ch;
		switch (de->d_type) {
		case DT_DIR:
			type_ch = 'd';
			break;
		case DT_REG:
			type_ch = '-';
			break;
		case DT_LNK:
			type_ch = 'l';
			break;
		default:
			type_ch = '?';
			break;
		}

		uint64_t size = 0;
		if (de->d_type == DT_REG) {
			char entry_path[4096];
			snprintf(entry_path, sizeof(entry_path), "%s/%s", fpath,
				 de->d_name);
			struct lkl_stat st;
			if (lkl_sys_lstat(entry_path, &st) == 0)
				size = (uint64_t)st.st_size;
		}

		char sbuf[32];
		human_size(size, sbuf, sizeof(sbuf));
		printf("%c %8s %s\n", type_ch, sbuf, de->d_name);
	}
	lkl_closedir(dir);
	return 0;
}

static int cmd_cat(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: cat <path>\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(argv[1], gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	long fd = lkl_sys_open(fpath, LKL_O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", gpath);
		return -1;
	}

	char buf[4096];
	long n;
	while ((n = lkl_sys_read(fd, buf, sizeof(buf))) > 0)
		fwrite(buf, 1, (size_t)n, stdout);
	lkl_sys_close(fd);
	return 0;
}

static int cmd_stat(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: stat <path>\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(argv[1], gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	struct lkl_stat st;
	long ret = lkl_sys_lstat(fpath, &st);
	if (ret < 0) {
		fprintf(stderr, "Cannot stat: %s\n", gpath);
		return -1;
	}

	const char* type = "unknown";
	if (LKL_S_ISREG(st.st_mode))
		type = "regular file";
	else if (LKL_S_ISDIR(st.st_mode))
		type = "directory";
	else if (LKL_S_ISLNK(st.st_mode))
		type = "symbolic link";
	else if (LKL_S_ISBLK(st.st_mode))
		type = "block device";
	else if (LKL_S_ISCHR(st.st_mode))
		type = "character device";

	char sbuf[32];
	printf("  Path: %s\n", gpath);
	printf("  Type: %s\n", type);
	printf("  Size: %lld (%s)\n", (long long)st.st_size,
	       human_size((uint64_t)st.st_size, sbuf, sizeof(sbuf)));
	printf("  Inode: %llu\n", (unsigned long long)st.st_ino);
	printf("  Links: %llu\n", (unsigned long long)st.st_nlink);
	printf("  Mode: %04o\n", (unsigned)(st.st_mode & 07777));
	printf("  Uid: %u  Gid: %u\n", (unsigned)st.st_uid,
	       (unsigned)st.st_gid);
	return 0;
}

static int cmd_hexdump(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: hexdump <path> [offset] [length]\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(argv[1], gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	int64_t offset = argc >= 3 ? atoll(argv[2]) : 0;
	int64_t length = argc >= 4 ? atoll(argv[3]) : 256;

	long fd = lkl_sys_open(fpath, LKL_O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", gpath);
		return -1;
	}

	if (offset > 0)
		lkl_sys_lseek(fd, offset, LKL_SEEK_SET);

	uint8_t buf[16];
	int64_t pos = offset;
	while (length > 0) {
		int64_t chunk = length > 16 ? 16 : length;
		long n = lkl_sys_read(fd, buf, (int)chunk);
		if (n <= 0)
			break;

		printf("%08llx  ", (long long)pos);
		for (int i = 0; i < (int)n; i++)
			printf("%02x ", buf[i]);
		for (int i = (int)n; i < 16; i++)
			printf("   ");
		printf(" |");
		for (int i = 0; i < (int)n; i++)
			putchar(isprint(buf[i]) ? buf[i] : '.');
		printf("|\n");

		pos += n;
		length -= n;
	}
	lkl_sys_close(fd);
	return 0;
}

static int cmd_head(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	int nlines = 10;
	const char* filepath = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			nlines = atoi(argv[++i]);
		else
			filepath = argv[i];
	}
	if (!filepath) {
		fprintf(stderr, "Usage: head [-n N] <path>\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(filepath, gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	long fd = lkl_sys_open(fpath, LKL_O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", gpath);
		return -1;
	}

	int lines_printed = 0;
	char buf[4096];
	long n;
	while (lines_printed < nlines &&
	       (n = lkl_sys_read(fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n && lines_printed < nlines; i++) {
			putchar(buf[i]);
			if (buf[i] == '\n')
				lines_printed++;
		}
	}
	lkl_sys_close(fd);
	return 0;
}

static int cmd_tail(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	int nlines = 10;
	const char* filepath = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			nlines = atoi(argv[++i]);
		else
			filepath = argv[i];
	}
	if (!filepath) {
		fprintf(stderr, "Usage: tail [-n N] <path>\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(filepath, gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	long fd = lkl_sys_open(fpath, LKL_O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", gpath);
		return -1;
	}

	/* Read entire file to get last N lines */
	size_t cap = 65536, len = 0;
	char* data = malloc(cap);
	if (!data) {
		lkl_sys_close(fd);
		return -1;
	}

	long n;
	while ((n = lkl_sys_read(fd, data + len, cap - len)) > 0) {
		len += (size_t)n;
		if (len >= cap) {
			cap *= 2;
			char* tmp = realloc(data, cap);
			if (!tmp) {
				free(data);
				lkl_sys_close(fd);
				return -1;
			}
			data = tmp;
		}
	}
	lkl_sys_close(fd);

	int count = 0;
	size_t start = len;
	while (start > 0 && count <= nlines) {
		start--;
		if (data[start] == '\n')
			count++;
	}
	if (start > 0 || (start == 0 && count > nlines))
		start++;

	fwrite(data + start, 1, len - start, stdout);
	free(data);
	return 0;
}

static int cmd_download(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 3) {
		fprintf(stderr, "Usage: download <remote-path> <local-path>\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(argv[1], gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	long fd = lkl_sys_open(fpath, LKL_O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", gpath);
		return -1;
	}

	FILE* out = fopen(argv[2], "wb");
	if (!out) {
		fprintf(stderr, "Cannot create local file: %s: %s\n", argv[2],
			strerror(errno));
		lkl_sys_close(fd);
		return -1;
	}

	char buf[65536];
	long n;
	int64_t total = 0;
	while ((n = lkl_sys_read(fd, buf, sizeof(buf))) > 0) {
		fwrite(buf, 1, (size_t)n, out);
		total += n;
	}
	fclose(out);
	lkl_sys_close(fd);

	char sbuf[32];
	printf("%s -> %s (%s)\n", gpath, argv[2],
	       human_size((uint64_t)total, sbuf, sizeof(sbuf)));
	return 0;
}

static int cmd_df(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	struct lkl_statfs st;
	long ret = lkl_sys_statfs(g_mount_point, &st);
	if (ret < 0) {
		fprintf(stderr, "statfs failed: %ld\n", ret);
		return -1;
	}

	uint64_t total = (uint64_t)st.f_blocks * st.f_bsize;
	uint64_t free_b = (uint64_t)st.f_bfree * st.f_bsize;
	uint64_t avail = (uint64_t)st.f_bavail * st.f_bsize;
	uint64_t used = total - free_b;

	printf("Filesystem: %s (%s)\n", g_image_path, g_fstype);
	printf("Size:       %lu MB\n", (unsigned long)(total / (1024 * 1024)));
	printf("Used:       %lu MB\n", (unsigned long)(used / (1024 * 1024)));
	printf("Available:  %lu MB\n", (unsigned long)(avail / (1024 * 1024)));
	if (total > 0)
		printf("Use%%:       %lu%%\n",
		       (unsigned long)(used * 100 / total));
	printf("Inodes:     %lu (free: %lu)\n", (unsigned long)st.f_files,
	       (unsigned long)st.f_ffree);
	return 0;
}

static void find_recursive(const char* fpath, const char* gpath, int depth)
{
	int err;
	struct lkl_dir* dir = lkl_opendir(fpath, &err);
	if (!dir)
		return;

	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;

		char gfull[4096], ffull[4096];
		if (strcmp(gpath, "/") == 0)
			snprintf(gfull, sizeof(gfull), "/%s", de->d_name);
		else
			snprintf(gfull, sizeof(gfull), "%s/%s", gpath,
				 de->d_name);
		snprintf(ffull, sizeof(ffull), "%s/%s", fpath, de->d_name);

		printf("%s\n", gfull);

		if (de->d_type == DT_DIR && depth < 64)
			find_recursive(ffull, gfull, depth + 1);
	}
	lkl_closedir(dir);
}

static int cmd_find(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(argc >= 2 ? argv[1] : NULL, gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	find_recursive(fpath, gpath, 0);
	return 0;
}

static int cmd_cd(int argc, char** argv)
{
	if (!g_mounted) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: cd <path>\n");
		return -1;
	}

	char gpath[4096], fpath[4096];
	resolve_path(argv[1], gpath, sizeof(gpath));
	full_path(gpath, fpath, sizeof(fpath));

	/* Verify it's a directory */
	struct lkl_stat st;
	long ret = lkl_sys_lstat(fpath, &st);
	if (ret < 0 || !LKL_S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Not a directory: %s\n", gpath);
		return -1;
	}

	snprintf(g_cwd, sizeof(g_cwd), "%s", gpath);
	return 0;
}

static int cmd_lcd(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: lcd <path>\n");
		return -1;
	}
	if (chdir(argv[1]) < 0) {
		fprintf(stderr, "lcd: %s: %s\n", argv[1], strerror(errno));
		return -1;
	}
	char cwd[4096];
	if (getcwd(cwd, sizeof(cwd)))
		printf("%s\n", cwd);
	return 0;
}

static int cmd_pwd(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	printf("%s\n", g_cwd);
	return 0;
}

static int cmd_help(int argc, char** argv)
{
	if (argc >= 2) {
		for (struct command* c = commands; c->name; c++) {
			if (strcmp(c->name, argv[1]) == 0) {
				printf("  %s\n  %s\n", c->usage, c->help);
				return 0;
			}
		}
		fprintf(stderr, "Unknown command: %s\n", argv[1]);
		return -1;
	}
	printf("Commands:\n");
	for (struct command* c = commands; c->name; c++)
		printf("  %-28s %s\n", c->usage, c->help);
	return 0;
}

static int cmd_quit(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	return 1;
}

/* --- Readline completion --- */

static char* command_generator(const char* text, int state)
{
	static int idx;
	if (!state)
		idx = 0;

	size_t len = strlen(text);
	while (commands[idx].name) {
		const char* name = commands[idx].name;
		idx++;
		if (strncmp(name, text, len) == 0)
			return strdup(name);
	}
	return NULL;
}

static char* path_generator(const char* text, int state)
{
	static struct lkl_dir* dir;
	static char dir_fpath[4096];
	static char prefix[4096];
	static size_t prefix_len;

	if (!state) {
		char resolved[4096];
		resolve_path(text, resolved, sizeof(resolved));

		char dir_gpath[4096];
		char* slash = strrchr(resolved, '/');
		if (slash == resolved) {
			strcpy(dir_gpath, "/");
			snprintf(prefix, sizeof(prefix), "%s", resolved + 1);
		} else if (slash) {
			*slash = '\0';
			strcpy(dir_gpath, resolved);
			snprintf(prefix, sizeof(prefix), "%s", slash + 1);
		} else {
			resolve_path("", dir_gpath, sizeof(dir_gpath));
			snprintf(prefix, sizeof(prefix), "%s", resolved);
		}
		prefix_len = strlen(prefix);

		full_path(dir_gpath, dir_fpath, sizeof(dir_fpath));
		if (dir)
			lkl_closedir(dir);
		int err;
		dir = g_mounted ? lkl_opendir(dir_fpath, &err) : NULL;
	}

	if (!dir)
		return NULL;

	struct lkl_linux_dirent64* de;
	while ((de = lkl_readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (strncmp(de->d_name, prefix, prefix_len) == 0) {
			char result[4096];
			const char* last_slash = strrchr(text, '/');
			if (last_slash) {
				size_t dlen = (size_t)(last_slash - text + 1);
				snprintf(result, dlen + 1, "%s", text);
				snprintf(result + dlen, sizeof(result) - dlen,
					 "%s", de->d_name);
			} else {
				snprintf(result, sizeof(result), "%s",
					 de->d_name);
			}
			if (de->d_type == DT_DIR) {
				size_t rlen = strlen(result);
				result[rlen] = '/';
				result[rlen + 1] = '\0';
			}
			return strdup(result);
		}
	}

	lkl_closedir(dir);
	dir = NULL;
	return NULL;
}

static char** shell_completion(const char* text, int start, int end)
{
	(void)end;
	rl_attempted_completion_over = 1;

	if (start == 0)
		return rl_completion_matches(text, command_generator);

	if (g_mounted)
		return rl_completion_matches(text, path_generator);

	return NULL;
}

/* --- Tokenizer --- */

static int tokenize(char* line, char** argv, int max_args)
{
	int argc = 0;
	char* p = line;

	while (*p && argc < max_args - 1) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;

		if (*p == '"') {
			p++;
			argv[argc++] = p;
			while (*p && *p != '"')
				p++;
			if (*p)
				*p++ = '\0';
		} else {
			argv[argc++] = p;
			while (*p && !isspace((unsigned char)*p))
				p++;
			if (*p)
				*p++ = '\0';
		}
	}
	argv[argc] = NULL;
	return argc;
}

/* --- Main --- */

static void usage(const char* prog)
{
	fprintf(stderr, "Usage: %s [options] [image fstype [part]]\n", prog);
	fprintf(stderr, "Options:\n");
	fprintf(stderr,
		"  -v, --verbose   Show kernel messages (loglevel=7)\n");
	fprintf(stderr, "  -m MB           Set kernel memory (default: 64)\n");
	fprintf(stderr, "  -h, --help      Show this help\n");
}

int main(int argc, char** argv)
{
	AnyfsKernelOpts opts = {.mem_mb = 64, .loglevel = 0};

	int argi = 1;
	while (argi < argc && argv[argi][0] == '-') {
		if (strcmp(argv[argi], "-v") == 0 ||
		    strcmp(argv[argi], "--verbose") == 0) {
			opts.loglevel = 7;
			argi++;
		} else if (strcmp(argv[argi], "-m") == 0 && argi + 1 < argc) {
			opts.mem_mb = (uint32_t)atoi(argv[++argi]);
			argi++;
		} else if (strcmp(argv[argi], "-h") == 0 ||
			   strcmp(argv[argi], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[argi]);
			usage(argv[0]);
			return 1;
		}
	}

	if (anyfs_kernel_init(&opts) < 0) {
		fprintf(stderr, "Kernel init failed\n");
		return 1;
	}

	/* If args provided on command line: open <image> <fstype> [part] */
	if (argi + 1 < argc) {
		char* args_open[] = {"open", argv[argi], NULL};
		cmd_open(2, args_open);

		char part_str[16] = "0";
		if (argi + 2 < argc)
			snprintf(part_str, sizeof(part_str), "%s",
				 argv[argi + 2]);
		char* args_mount[] = {"mount", argv[argi + 1], part_str, NULL};
		cmd_mount(3, args_mount);
	}

	rl_readline_name = "anyfs";
	rl_attempted_completion_function = shell_completion;
	using_history();

	char* history_file = NULL;
	const char* home = getenv("HOME");
	if (home) {
		size_t len = strlen(home) + 20;
		history_file = malloc(len);
		snprintf(history_file, len, "%s/.anyfs_history", home);
		read_history(history_file);
	}

	printf("anyfs-shell v0.2 — type 'help' for commands\n");

	char prompt[512];
	char* line;
	for (;;) {
		if (g_mounted) {
			const char* basename = strrchr(g_image_path, '/');
			basename = basename ? basename + 1 : g_image_path;
			snprintf(prompt, sizeof(prompt), "<%s:%s %s> ",
				 basename, g_fstype, g_cwd);
		} else if (g_image_path[0]) {
			const char* basename = strrchr(g_image_path, '/');
			basename = basename ? basename + 1 : g_image_path;
			snprintf(prompt, sizeof(prompt), "<%s> ", basename);
		} else {
			snprintf(prompt, sizeof(prompt), "><anyfs> ");
		}

		line = readline(prompt);
		if (!line)
			break;
		char* trimmed = line;
		while (*trimmed && isspace((unsigned char)*trimmed))
			trimmed++;
		if (!*trimmed) {
			free(line);
			continue;
		}

		add_history(line);

		if (trimmed[0] == '!') {
			system(trimmed + 1);
			free(line);
			continue;
		}

		char* cmd_argv[64];
		int cmd_argc = tokenize(trimmed, cmd_argv, 64);
		if (cmd_argc == 0) {
			free(line);
			continue;
		}

		int found = 0;
		for (struct command* c = commands; c->name; c++) {
			if (strcmp(c->name, cmd_argv[0]) == 0) {
				int ret = c->fn(cmd_argc, cmd_argv);
				if (ret == 1) {
					free(line);
					goto done;
				}
				found = 1;
				break;
			}
		}
		if (!found)
			fprintf(stderr, "Unknown command: %s (type 'help')\n",
				cmd_argv[0]);

		free(line);
	}

done:
	if (history_file) {
		write_history(history_file);
		free(history_file);
	}
	if (g_mounted) {
		anyfs_umount("shell");
	}
	if (g_disk_id >= 0)
		anyfs_disk_remove(g_disk_id);
	anyfs_kernel_halt();
	return 0;
}
