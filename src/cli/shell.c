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

#include "anyfs_api.h"

/* Shell state */
static AnyfsContext* g_ctx;
static AnyfsMount* g_mnt;
static char g_cwd[4096] = "/";
static char g_image_path[4096];
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
static int cmd_cat(int argc, char** argv);
static int cmd_stat(int argc, char** argv);
static int cmd_hexdump(int argc, char** argv);
static int cmd_find(int argc, char** argv);
static int cmd_cd(int argc, char** argv);
static int cmd_pwd(int argc, char** argv);
static int cmd_help(int argc, char** argv);
static int cmd_quit(int argc, char** argv);

static struct command commands[] = {
    {"open", "open <image> [flags]", "Open a disk image (flags: ro, gio, qemu)",
     cmd_open},
    {"mount", "mount <fstype> [part]",
     "Mount filesystem (e.g. ext4, xfs, btrfs)", cmd_mount},
    {"umount", "umount", "Unmount current filesystem", cmd_umount},
    {"ls", "ls [path]", "List directory contents", cmd_ls},
    {"cat", "cat <path>", "Print file contents", cmd_cat},
    {"stat", "stat <path>", "Show file information", cmd_stat},
    {"hexdump", "hexdump <path> [off] [len]", "Hex dump of file", cmd_hexdump},
    {"find", "find [path]", "Recursive file listing", cmd_find},
    {"cd", "cd <path>", "Change working directory", cmd_cd},
    {"pwd", "pwd", "Print working directory", cmd_pwd},
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
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "gio") == 0)
			flags |= ANYFS_OPEN_GIO;
		else if (strcmp(argv[i], "qemu") == 0)
			flags |= ANYFS_OPEN_QEMU;
	}

	/* Auto-detect QEMU backend from extension */
	if (!(flags & (ANYFS_OPEN_GIO | ANYFS_OPEN_QEMU))) {
		const char* ext = strrchr(argv[1], '.');
		if (ext &&
		    (strcmp(ext, ".qcow2") == 0 || strcmp(ext, ".vmdk") == 0 ||
		     strcmp(ext, ".vdi") == 0 || strcmp(ext, ".vhdx") == 0 ||
		     strcmp(ext, ".vpc") == 0))
			flags |= ANYFS_OPEN_QEMU;
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
		const char* type_str;
		switch (entry.type) {
		case 4:
			type_str = "DIR ";
			break;
		case 8:
			type_str = "FILE";
			break;
		case 10:
			type_str = "LINK";
			break;
		default:
			type_str = "??? ";
			break;
		}
		printf("  [%s] %s\n", type_str, entry.name);
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

	/* Use opendir to check if directory, open+read for file size */
	AnyfsDir* dir = anyfs_opendir(g_mnt, path);
	if (dir) {
		printf("  %s: directory\n", path);
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

	printf("  %s: file, %lld bytes\n", path, (long long)total);
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

	char* line;
	while ((line = readline(g_mounted ? "anyfs> " : "anyfs[no mount]> ")) !=
	       NULL) {
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
