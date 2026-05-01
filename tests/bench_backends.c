/*
 * Benchmark: compare latency of raw vs gio vs gio-async backends.
 * Each backend runs in a separate child process (LKL can only init once).
 *
 * Usage: bench_backends <image> <fstype> <part> [iterations]
 */
#include "anyfs_api.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double time_diff_ms(struct timespec* start, struct timespec* end)
{
	double s = (double)(end->tv_sec - start->tv_sec) * 1000.0;
	double ns = (double)(end->tv_nsec - start->tv_nsec) / 1e6;
	return s + ns;
}

typedef struct {
	const char* name;
	uint32_t flags;
} BackendDef;

static int bench_one(const char* image, const char* fstype, uint32_t part,
		     int iterations, const BackendDef* backend)
{
	struct timespec t_init_start, t_init_end;
	struct timespec t_mount_start, t_mount_end;
	struct timespec t_read_start, t_read_end;
	struct timespec t_total_start, t_total_end;

	clock_gettime(CLOCK_MONOTONIC, &t_total_start);

	/* Init */
	clock_gettime(CLOCK_MONOTONIC, &t_init_start);
	AnyfsContext* ctx = NULL;
	int32_t ret = anyfs_init(&ctx);
	if (ret != ANYFS_OK) {
		fprintf(stderr, "[%s] anyfs_init failed: %d\n", backend->name,
			ret);
		return -1;
	}

	ret = anyfs_open_image(ctx, image, backend->flags);
	if (ret != ANYFS_OK) {
		fprintf(stderr, "[%s] anyfs_open_image failed: %d\n",
			backend->name, ret);
		anyfs_destroy(ctx);
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t_init_end);

	/* Mount */
	clock_gettime(CLOCK_MONOTONIC, &t_mount_start);
	AnyfsMount* mnt = NULL;
	ret = anyfs_mount(ctx, fstype, part, &mnt);
	if (ret != ANYFS_OK) {
		fprintf(stderr, "[%s] mount failed: %d\n", backend->name, ret);
		anyfs_destroy(ctx);
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t_mount_end);

	/* Repeated read benchmark */
	clock_gettime(CLOCK_MONOTONIC, &t_read_start);
	char buf[4096];
	for (int i = 0; i < iterations; i++) {
		anyfs_fd_t fd = anyfs_open(mnt, "hello.txt", 0);
		if (fd < 0) {
			AnyfsDir* dir = anyfs_opendir(mnt, "");
			if (dir) {
				AnyfsEntry entry;
				while (anyfs_readdir(dir, &entry) == ANYFS_OK)
					;
				anyfs_closedir(dir);
			}
			continue;
		}
		int64_t n = anyfs_read(mnt, fd, buf, sizeof(buf));
		(void)n;
		anyfs_close(mnt, fd);
	}
	clock_gettime(CLOCK_MONOTONIC, &t_read_end);

	/* Cleanup */
	anyfs_umount(mnt);
	anyfs_destroy(ctx);
	clock_gettime(CLOCK_MONOTONIC, &t_total_end);

	double init_ms = time_diff_ms(&t_init_start, &t_init_end);
	double mount_ms = time_diff_ms(&t_mount_start, &t_mount_end);
	double read_ms = time_diff_ms(&t_read_start, &t_read_end);
	double total_ms = time_diff_ms(&t_total_start, &t_total_end);

	printf("  %-12s | init: %7.2f ms | mount: %7.2f ms | "
	       "%d reads: %7.2f ms (%6.3f ms/op) | total: %7.2f ms\n",
	       backend->name, init_ms, mount_ms, iterations, read_ms,
	       read_ms / iterations, total_ms);
	fflush(stdout);

	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <image> <fstype> <part> [iterations]\n",
			argv[0]);
		return 1;
	}

	const char* image = argv[1];
	const char* fstype = argv[2];
	uint32_t part = (uint32_t)atoi(argv[3]);
	int iterations = (argc > 4) ? atoi(argv[4]) : 100;

	BackendDef backends[] = {
	    {"raw", ANYFS_OPEN_READONLY},
	    {"aio", ANYFS_OPEN_READONLY | ANYFS_OPEN_AIO},
#ifdef ANYFS_HAS_GIO
	    {"gio-sync", ANYFS_OPEN_READONLY | ANYFS_OPEN_GIO},
	    {"gio-async", ANYFS_OPEN_READONLY | ANYFS_OPEN_GIO_ASYNC},
#endif
	};
	int n_backends = sizeof(backends) / sizeof(backends[0]);

	printf("Benchmark: %s (fs=%s, part=%u, iterations=%d)\n", image, fstype,
	       part, iterations);
	printf("%-14s | %-18s | %-18s | %-36s | %s\n", "  Backend", "Init",
	       "Mount", "Reads", "Total");
	printf("  "
	       "---------------------------------------------------------------"
	       "------------"
	       "------------------------------\n");
	fflush(stdout);

	/* Each backend runs in a child process (LKL can only init once) */
	for (int i = 0; i < n_backends; i++) {
		int pipefd[2];
		if (pipe(pipefd) < 0) {
			perror("pipe");
			return 1;
		}

		pid_t pid = fork();
		if (pid == 0) {
			/* Child: suppress ALL stdout/stderr (LKL prints there)
			 */
			close(pipefd[0]);
			int result_fd =
			    pipefd[1]; /* keep pipe open for result */
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull >= 0) {
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
			/* Run benchmark (its printf goes to /dev/null) */
			/* We capture result ourselves via result_fd */
			struct timespec ts, te, tm1, tm2, tr1, tr2, tt1, tt2;
			clock_gettime(CLOCK_MONOTONIC, &tt1);
			clock_gettime(CLOCK_MONOTONIC, &ts);
			AnyfsContext* ctx = NULL;
			int32_t ret = anyfs_init(&ctx);
			if (ret != ANYFS_OK) {
				_exit(1);
			}
			ret = anyfs_open_image(ctx, image, backends[i].flags);
			if (ret != ANYFS_OK) {
				anyfs_destroy(ctx);
				_exit(1);
			}
			clock_gettime(CLOCK_MONOTONIC, &te);

			clock_gettime(CLOCK_MONOTONIC, &tm1);
			AnyfsMount* mnt = NULL;
			ret = anyfs_mount(ctx, fstype, part, &mnt);
			if (ret != ANYFS_OK) {
				anyfs_destroy(ctx);
				_exit(1);
			}
			clock_gettime(CLOCK_MONOTONIC, &tm2);

			clock_gettime(CLOCK_MONOTONIC, &tr1);
			char buf[4096];
			/*
			 * Read strategy: open a large file and read sequential
			 * 4K blocks. This ensures page cache misses (LKL has
			 * only 64MB memory). If no large file exists, fall back
			 * to repeated small file reads (measures page cache
			 * path — still valid for VFS overhead).
			 */
			anyfs_fd_t fd = anyfs_open(mnt, "bigfile.bin", 0);
			if (fd >= 0) {
				/* Large file mode: sequential read, each 4K
				 * block */
				for (int j = 0; j < iterations; j++) {
					int64_t n = anyfs_read(mnt, fd, buf,
							       sizeof(buf));
					if (n <= 0) {
						/* EOF or error: seek back to
						 * start */
						anyfs_close(mnt, fd);
						fd = anyfs_open(
						    mnt, "bigfile.bin", 0);
						if (fd < 0)
							break;
					}
				}
				anyfs_close(mnt, fd);
			} else {
				/* Small file fallback: open/read/close cycle
				 * (page cache hits) */
				for (int j = 0; j < iterations; j++) {
					fd = anyfs_open(mnt, "hello.txt", 0);
					if (fd >= 0) {
						anyfs_read(mnt, fd, buf,
							   sizeof(buf));
						anyfs_close(mnt, fd);
					}
				}
			}
			clock_gettime(CLOCK_MONOTONIC, &tr2);

			anyfs_umount(mnt);
			anyfs_destroy(ctx);
			clock_gettime(CLOCK_MONOTONIC, &tt2);

			double init_ms = time_diff_ms(&ts, &te);
			double mount_ms = time_diff_ms(&tm1, &tm2);
			double read_ms = time_diff_ms(&tr1, &tr2);
			double total_ms = time_diff_ms(&tt1, &tt2);

			char line[256];
			int len = snprintf(
			    line, sizeof(line),
			    "  %-12s | init: %7.2f ms | mount: %7.2f ms | "
			    "%d reads: %7.2f ms (%6.3f ms/op) | total: %7.2f "
			    "ms\n",
			    backends[i].name, init_ms, mount_ms, iterations,
			    read_ms, read_ms / iterations, total_ms);
			(void)write(result_fd, line, (size_t)len);
			close(result_fd);
			_exit(0);
		} else if (pid > 0) {
			close(pipefd[1]);
			char result[512];
			ssize_t n = read(pipefd[0], result, sizeof(result) - 1);
			close(pipefd[0]);
			int status;
			waitpid(pid, &status, 0);
			if (n > 0) {
				result[n] = '\0';
				printf("%s", result);
			} else {
				printf("  %-12s | FAILED\n", backends[i].name);
			}
			fflush(stdout);
		} else {
			perror("fork");
			return 1;
		}
	}

	return 0;
}
