/*
 * anyfs-shell: interactive filesystem image explorer
 * Uses anyfs_api.h to mount and browse filesystem images.
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

#include "anyfs_api.h"

/* Shell state */
static AnyfsContext* g_ctx;
static AnyfsMount* g_mnt;
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
    {"open", "open <image> [flags]",
     "Open a disk image (flags: raw, gio, qemu)", cmd_open},
    {"mount", "mount <fstype> [part]",
     "Mount filesystem (e.g. ext4, xfs, btrfs)", cmd_mount},
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
	/* Simple normalization: remove trailing slash */
	size_t len = strlen(out);
	while (len > 1 && out[len - 1] == '/')
		out[--len] = '\0';
}

/* --- Command implementations --- */

static int cmd_open(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: open <image> [flags...]\n");
		return -1;
	}
	if (g_mnt) {
		fprintf(stderr, "Already mounted. Use 'umount' first.\n");
		return -1;
	}

	uint32_t flags = ANYFS_OPEN_READONLY;
	int explicit_backend = 0;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "gio") == 0) {
			flags |= ANYFS_OPEN_GIO;
			explicit_backend = 1;
		} else if (strcmp(argv[i], "qemu") == 0) {
			flags |= ANYFS_OPEN_QEMU;
			explicit_backend = 1;
		} else if (strcmp(argv[i], "raw") == 0) {
			explicit_backend = 1; /* explicitly use raw */
		}
	}

	/* Default backend: prefer QEMU if available, else raw */
	if (!explicit_backend) {
#ifdef ANYFS_HAS_QEMU
		flags |= ANYFS_OPEN_QEMU;
#endif
	}

	int32_t rc = anyfs_open_image(g_ctx, argv[1], flags);
	if (rc != ANYFS_OK) {
		fprintf(stderr, "Failed to open image: error %d\n", rc);
		return -1;
	}
	snprintf(g_image_path, sizeof(g_image_path), "%s", argv[1]);
	printf("Opened: %s\n", argv[1]);
	return 0;
}

static int cmd_mount(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: mount <fstype> [partition_index]\n");
		return -1;
	}
	if (g_mnt) {
		fprintf(stderr, "Already mounted. Use 'umount' first.\n");
		return -1;
	}

	uint32_t part = 0;
	if (argc >= 3)
		part = (uint32_t)atoi(argv[2]);

	int32_t rc = anyfs_mount(g_ctx, argv[1], part, &g_mnt);
	if (rc != ANYFS_OK) {
		fprintf(stderr, "Mount failed: error %d\n", rc);
		return -1;
	}
	g_mounted = 1;
	snprintf(g_fstype, sizeof(g_fstype), "%s", argv[1]);
	strcpy(g_cwd, "/");
	printf("Mounted %s (partition %u)\n", argv[1], part);
	return 0;
}

static int cmd_umount(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	anyfs_umount(g_mnt);
	g_mnt = NULL;
	g_mounted = 0;
	g_fstype[0] = '\0';
	strcpy(g_cwd, "/");
	printf("Unmounted.\n");
	return 0;
}

static int cmd_ls(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	char path[4096];
	resolve_path(argc >= 2 ? argv[1] : NULL, path, sizeof(path));

	AnyfsDir* dir = anyfs_opendir(g_mnt, path);
	if (!dir) {
		fprintf(stderr, "Cannot open directory: %s\n", path);
		return -1;
	}

	AnyfsEntry entry;
	while (anyfs_readdir(dir, &entry) == ANYFS_OK) {
		char indicator = ' ';
		switch (entry.type) {
		case 4:
			indicator = '/';
			break;
		case 10:
			indicator = '@';
			break;
		}
		if (entry.type == 4)
			printf("%s/\n", entry.name);
		else if (entry.type == 10)
			printf("%s@\n", entry.name);
		else
			printf("%s\n", entry.name);
		(void)indicator;
	}
	anyfs_closedir(dir);
	return 0;
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

static int cmd_ll(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	char path[4096];
	resolve_path(argc >= 2 ? argv[1] : NULL, path, sizeof(path));

	AnyfsDir* dir = anyfs_opendir(g_mnt, path);
	if (!dir) {
		fprintf(stderr, "Cannot open directory: %s\n", path);
		return -1;
	}

	AnyfsEntry entry;
	while (anyfs_readdir(dir, &entry) == ANYFS_OK) {
		char type_ch;
		switch (entry.type) {
		case 4:
			type_ch = 'd';
			break;
		case 8:
			type_ch = '-';
			break;
		case 10:
			type_ch = 'l';
			break;
		default:
			type_ch = '?';
			break;
		}

		/* Get actual size for regular files */
		uint64_t size = entry.size;
		if (entry.type == 8 && size == 0) {
			char full[4096];
			if (strcmp(path, "/") == 0)
				snprintf(full, sizeof(full), "/%s", entry.name);
			else
				snprintf(full, sizeof(full), "%s/%s", path,
					 entry.name);
			anyfs_fd_t fd = anyfs_open(g_mnt, full, 0);
			if (fd >= 0) {
				char rbuf[65536];
				int64_t n;
				while ((n = anyfs_read(g_mnt, fd, rbuf,
						       sizeof(rbuf))) > 0)
					size += (uint64_t)n;
				anyfs_close(g_mnt, fd);
			}
		}

		char sbuf[32];
		human_size(size, sbuf, sizeof(sbuf));
		printf("%c %8s %s\n", type_ch, sbuf, entry.name);
	}
	anyfs_closedir(dir);
	return 0;
}

static int cmd_cat(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: cat <path>\n");
		return -1;
	}

	char path[4096];
	resolve_path(argv[1], path, sizeof(path));

	anyfs_fd_t fd = anyfs_open(g_mnt, path, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", path);
		return -1;
	}

	char buf[4096];
	int64_t n;
	while ((n = anyfs_read(g_mnt, fd, buf, sizeof(buf))) > 0) {
		fwrite(buf, 1, (size_t)n, stdout);
	}
	anyfs_close(g_mnt, fd);
	return 0;
}

static int cmd_stat(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: stat <path>\n");
		return -1;
	}

	char path[4096];
	resolve_path(argv[1], path, sizeof(path));

	/* Try as directory first */
	AnyfsDir* dir = anyfs_opendir(g_mnt, path);
	if (dir) {
		printf("  Path: %s\n", path);
		printf("  Type: directory\n");
		/* Count entries */
		AnyfsEntry entry;
		int count = 0;
		while (anyfs_readdir(dir, &entry) == ANYFS_OK)
			count++;
		printf("  Entries: %d\n", count);
		anyfs_closedir(dir);
		return 0;
	}

	anyfs_fd_t fd = anyfs_open(g_mnt, path, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot stat: %s (not found)\n", path);
		return -1;
	}

	/* Read to determine size */
	char buf[65536];
	int64_t total = 0, n;
	while ((n = anyfs_read(g_mnt, fd, buf, sizeof(buf))) > 0)
		total += n;
	anyfs_close(g_mnt, fd);

	char sbuf[32];
	printf("  Path: %s\n", path);
	printf("  Type: regular file\n");
	printf("  Size: %lld (%s)\n", (long long)total,
	       human_size((uint64_t)total, sbuf, sizeof(sbuf)));
	return 0;
}

static int cmd_hexdump(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: hexdump <path> [offset] [length]\n");
		return -1;
	}

	char path[4096];
	resolve_path(argv[1], path, sizeof(path));

	int64_t offset = argc >= 3 ? atoll(argv[2]) : 0;
	int64_t length = argc >= 4 ? atoll(argv[3]) : 256;

	anyfs_fd_t fd = anyfs_open(g_mnt, path, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", path);
		return -1;
	}

	/* Skip to offset */
	char skip_buf[4096];
	int64_t to_skip = offset;
	while (to_skip > 0) {
		int64_t chunk = to_skip > (int64_t)sizeof(skip_buf)
				    ? (int64_t)sizeof(skip_buf)
				    : to_skip;
		int64_t n = anyfs_read(g_mnt, fd, skip_buf, (uint64_t)chunk);
		if (n <= 0)
			break;
		to_skip -= n;
	}

	/* Read and display */
	uint8_t buf[16];
	int64_t pos = offset;
	while (length > 0) {
		int64_t chunk = length > 16 ? 16 : length;
		int64_t n = anyfs_read(g_mnt, fd, buf, (uint64_t)chunk);
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
	anyfs_close(g_mnt, fd);
	return 0;
}

static int cmd_head(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	int nlines = 10;
	const char* filepath = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			nlines = atoi(argv[++i]);
		} else {
			filepath = argv[i];
		}
	}
	if (!filepath) {
		fprintf(stderr, "Usage: head [-n N] <path>\n");
		return -1;
	}

	char path[4096];
	resolve_path(filepath, path, sizeof(path));

	anyfs_fd_t fd = anyfs_open(g_mnt, path, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", path);
		return -1;
	}

	int lines_printed = 0;
	char buf[4096];
	int64_t n;
	while (lines_printed < nlines &&
	       (n = anyfs_read(g_mnt, fd, buf, sizeof(buf))) > 0) {
		for (int64_t i = 0; i < n && lines_printed < nlines; i++) {
			putchar(buf[i]);
			if (buf[i] == '\n')
				lines_printed++;
		}
	}
	anyfs_close(g_mnt, fd);
	return 0;
}

static int cmd_tail(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	int nlines = 10;
	const char* filepath = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			nlines = atoi(argv[++i]);
		} else {
			filepath = argv[i];
		}
	}
	if (!filepath) {
		fprintf(stderr, "Usage: tail [-n N] <path>\n");
		return -1;
	}

	char path[4096];
	resolve_path(filepath, path, sizeof(path));

	anyfs_fd_t fd = anyfs_open(g_mnt, path, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", path);
		return -1;
	}

	/* Read entire file into memory to get last N lines */
	size_t cap = 65536, len = 0;
	char* data = malloc(cap);
	if (!data) {
		anyfs_close(g_mnt, fd);
		return -1;
	}

	int64_t n;
	while ((n = anyfs_read(g_mnt, fd, data + len, cap - len)) > 0) {
		len += (size_t)n;
		if (len >= cap) {
			cap *= 2;
			char* tmp = realloc(data, cap);
			if (!tmp) {
				free(data);
				anyfs_close(g_mnt, fd);
				return -1;
			}
			data = tmp;
		}
	}
	anyfs_close(g_mnt, fd);

	/* Find the start of the last N lines */
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
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 3) {
		fprintf(stderr, "Usage: download <remote-path> <local-path>\n");
		return -1;
	}

	char path[4096];
	resolve_path(argv[1], path, sizeof(path));

	anyfs_fd_t fd = anyfs_open(g_mnt, path, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open: %s\n", path);
		return -1;
	}

	FILE* out = fopen(argv[2], "wb");
	if (!out) {
		fprintf(stderr, "Cannot create local file: %s: %s\n", argv[2],
			strerror(errno));
		anyfs_close(g_mnt, fd);
		return -1;
	}

	char buf[65536];
	int64_t n, total = 0;
	while ((n = anyfs_read(g_mnt, fd, buf, sizeof(buf))) > 0) {
		fwrite(buf, 1, (size_t)n, out);
		total += n;
	}
	fclose(out);
	anyfs_close(g_mnt, fd);

	char sbuf[32];
	printf("%s -> %s (%s)\n", path, argv[2],
	       human_size((uint64_t)total, sbuf, sizeof(sbuf)));
	return 0;
}

static int cmd_df(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	AnyfsStatvfs st;
	int32_t rc = anyfs_statvfs(g_mnt, &st);
	if (rc != ANYFS_OK) {
		fprintf(stderr, "statvfs failed: %d\n", rc);
		return -1;
	}

	uint64_t total = st.f_blocks * st.f_bsize;
	uint64_t free_b = st.f_bfree * st.f_bsize;
	uint64_t avail = st.f_bavail * st.f_bsize;
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

static void find_recursive(const char* path, int depth)
{
	AnyfsDir* dir = anyfs_opendir(g_mnt, path);
	if (!dir)
		return;

	AnyfsEntry entry;
	while (anyfs_readdir(dir, &entry) == ANYFS_OK) {
		if (strcmp(entry.name, ".") == 0 ||
		    strcmp(entry.name, "..") == 0)
			continue;

		char full[4096];
		if (strcmp(path, "/") == 0)
			snprintf(full, sizeof(full), "/%s", entry.name);
		else
			snprintf(full, sizeof(full), "%s/%s", path, entry.name);

		printf("%s\n", full);

		if (entry.type == 4 && depth < 64) /* DT_DIR */
			find_recursive(full, depth + 1);
	}
	anyfs_closedir(dir);
}

static int cmd_find(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}

	char path[4096];
	resolve_path(argc >= 2 ? argv[1] : NULL, path, sizeof(path));

	find_recursive(path, 0);
	return 0;
}

static int cmd_cd(int argc, char** argv)
{
	if (!g_mnt) {
		fprintf(stderr, "Not mounted.\n");
		return -1;
	}
	if (argc < 2) {
		fprintf(stderr, "Usage: cd <path>\n");
		return -1;
	}

	char path[4096];
	resolve_path(argv[1], path, sizeof(path));

	/* Verify it's a directory */
	AnyfsDir* dir = anyfs_opendir(g_mnt, path);
	if (!dir) {
		fprintf(stderr, "Not a directory: %s\n", path);
		return -1;
	}
	anyfs_closedir(dir);

	snprintf(g_cwd, sizeof(g_cwd), "%s", path);
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
	return 1; /* special: exit shell */
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
	static AnyfsDir* dir;
	static char dir_path[4096];
	static char prefix[4096];
	static size_t prefix_len;

	if (!state) {
		/* Split text into directory part and basename prefix */
		char resolved[4096];
		resolve_path(text, resolved, sizeof(resolved));

		/* Find last '/' */
		char* slash = strrchr(resolved, '/');
		if (slash == resolved) {
			strcpy(dir_path, "/");
			snprintf(prefix, sizeof(prefix), "%s", resolved + 1);
		} else if (slash) {
			*slash = '\0';
			strcpy(dir_path, resolved);
			snprintf(prefix, sizeof(prefix), "%s", slash + 1);
		} else {
			resolve_path("", dir_path, sizeof(dir_path));
			snprintf(prefix, sizeof(prefix), "%s", resolved);
		}
		prefix_len = strlen(prefix);

		if (dir)
			anyfs_closedir(dir);
		dir = g_mnt ? anyfs_opendir(g_mnt, dir_path) : NULL;
	}

	if (!dir)
		return NULL;

	AnyfsEntry entry;
	while (anyfs_readdir(dir, &entry) == ANYFS_OK) {
		if (strcmp(entry.name, ".") == 0 ||
		    strcmp(entry.name, "..") == 0)
			continue;
		if (strncmp(entry.name, prefix, prefix_len) == 0) {
			/* Build completion string relative to input */
			char result[4096];
			const char* input_dir = "";
			const char* last_slash = strrchr(text, '/');
			if (last_slash) {
				size_t dlen = (size_t)(last_slash - text + 1);
				snprintf(result, dlen + 1, "%s", text);
				snprintf(result + dlen, sizeof(result) - dlen,
					 "%s", entry.name);
			} else {
				snprintf(result, sizeof(result), "%s",
					 entry.name);
			}
			(void)input_dir;
			if (entry.type == 4) {
				size_t rlen = strlen(result);
				result[rlen] = '/';
				result[rlen + 1] = '\0';
			}
			return strdup(result);
		}
	}

	anyfs_closedir(dir);
	dir = NULL;
	return NULL;
}

static char** shell_completion(const char* text, int start, int end)
{
	(void)end;
	rl_attempted_completion_over = 1;

	if (start == 0)
		return rl_completion_matches(text, command_generator);

	if (g_mnt)
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
	AnyfsInitOpts opts = {
	    .size = sizeof(opts), .mem_mb = 64, .loglevel = 0};

	/* Parse options */
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

	int32_t rc = anyfs_init(&g_ctx, &opts);
	if (rc != ANYFS_OK) {
		fprintf(stderr, "anyfs_init failed: %d\n", rc);
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

	/* Setup readline */
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

	printf("anyfs-shell v0.1 — type 'help' for commands\n");

	char prompt[512];
	char* line;
	for (;;) {
		/* Build dynamic prompt */
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
		/* Skip empty lines */
		char* trimmed = line;
		while (*trimmed && isspace((unsigned char)*trimmed))
			trimmed++;
		if (!*trimmed) {
			free(line);
			continue;
		}

		add_history(line);

		/* Shell escape */
		if (trimmed[0] == '!') {
			system(trimmed + 1);
			free(line);
			continue;
		}

		/* Parse and dispatch */
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
				if (ret == 1) { /* quit */
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
	if (g_mnt)
		anyfs_umount(g_mnt);
	anyfs_destroy(g_ctx);
	return 0;
}
