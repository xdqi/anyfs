/*
 * Concurrent I/O benchmark: demonstrates async backend advantage.
 *
 * Spawns N pthreads that each read from the same file at different offsets.
 * With sync backends, each LKL thread blocks the vCPU during I/O (serialized).
 * With async backends, I/Os overlap (pipelined) while the vCPU runs other
 * threads.
 *
 * Each backend runs in a separate child process (LKL can only init once).
 *
 * Usage: bench_concurrent <image> <fstype> <part> [threads] [reads_per_thread]
 */
#include "anyfs_api.h"
#include <fcntl.h>
#include <pthread.h>
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

/* Thread argument */
typedef struct {
	AnyfsMount* mnt;
	int reads;
	int thread_id;
	double elapsed_ms;  /* output */
	int64_t bytes_read; /* output */
} ThreadArg;

static void* reader_thread(void* arg)
{
	ThreadArg* ta = arg;
	char buf[4096];
	struct timespec t1, t2;
	int64_t total_bytes = 0;

	clock_gettime(CLOCK_MONOTONIC, &t1);

	/* Each thread opens its own fd (different seek positions) */
	anyfs_fd_t fd = anyfs_open(ta->mnt, "bigfile.bin", 0);
	if (fd < 0) {
		/* Fall back to hello.txt */
		for (int i = 0; i < ta->reads; i++) {
			fd = anyfs_open(ta->mnt, "hello.txt", 0);
			if (fd >= 0) {
				int64_t n =
				    anyfs_read(ta->mnt, fd, buf, sizeof(buf));
				if (n > 0)
					total_bytes += n;
				anyfs_close(ta->mnt, fd);
			}
		}
	} else {
		for (int i = 0; i < ta->reads; i++) {
			int64_t n = anyfs_read(ta->mnt, fd, buf, sizeof(buf));
			if (n > 0) {
				total_bytes += n;
			} else {
				/* EOF: reopen */
				anyfs_close(ta->mnt, fd);
				fd = anyfs_open(ta->mnt, "bigfile.bin", 0);
				if (fd < 0)
					break;
			}
		}
		anyfs_close(ta->mnt, fd);
	}

	clock_gettime(CLOCK_MONOTONIC, &t2);
	ta->elapsed_ms = time_diff_ms(&t1, &t2);
	ta->bytes_read = total_bytes;
	return NULL;
}

static int run_bench(const char* image, const char* fstype, uint32_t part,
		     int n_threads, int reads_per_thread,
		     const BackendDef* backend, int result_fd)
{
	struct timespec t_total_start, t_total_end;

	clock_gettime(CLOCK_MONOTONIC, &t_total_start);

	AnyfsContext* ctx = NULL;
	int32_t ret = anyfs_init(&ctx);
	if (ret != ANYFS_OK)
		return -1;

	ret = anyfs_open_image(ctx, image, backend->flags);
	if (ret != ANYFS_OK) {
		anyfs_destroy(ctx);
		return -1;
	}

	AnyfsMount* mnt = NULL;
	ret = anyfs_mount(ctx, fstype, part, &mnt);
	if (ret != ANYFS_OK) {
		anyfs_destroy(ctx);
		return -1;
	}

	/* Spawn reader threads */
	pthread_t* threads = malloc(sizeof(pthread_t) * (size_t)n_threads);
	ThreadArg* args = malloc(sizeof(ThreadArg) * (size_t)n_threads);

	struct timespec t_read_start, t_read_end;
	clock_gettime(CLOCK_MONOTONIC, &t_read_start);

	for (int i = 0; i < n_threads; i++) {
		args[i].mnt = mnt;
		args[i].reads = reads_per_thread;
		args[i].thread_id = i;
		args[i].elapsed_ms = 0;
		args[i].bytes_read = 0;
		pthread_create(&threads[i], NULL, reader_thread, &args[i]);
	}

	for (int i = 0; i < n_threads; i++) {
		pthread_join(threads[i], NULL);
	}

	clock_gettime(CLOCK_MONOTONIC, &t_read_end);
	double read_ms = time_diff_ms(&t_read_start, &t_read_end);

	/* Compute total throughput */
	int64_t total_bytes = 0;
	double max_thread_ms = 0;
	for (int i = 0; i < n_threads; i++) {
		total_bytes += args[i].bytes_read;
		if (args[i].elapsed_ms > max_thread_ms)
			max_thread_ms = args[i].elapsed_ms;
	}

	anyfs_umount(mnt);
	anyfs_destroy(ctx);
	clock_gettime(CLOCK_MONOTONIC, &t_total_end);
	double total_ms = time_diff_ms(&t_total_start, &t_total_end);

	int total_ops = n_threads * reads_per_thread;
	double throughput_mb = (double)total_bytes / (1024.0 * 1024.0);
	double bw_mbs = throughput_mb / (read_ms / 1000.0);

	char line[512];
	int len =
	    snprintf(line, sizeof(line),
		     "  %-12s | %2d threads x %4d reads | wall: %7.2f ms | "
		     "%6.3f ms/op | %.1f MB/s | total: %7.2f ms\n",
		     backend->name, n_threads, reads_per_thread, read_ms,
		     read_ms / total_ops, bw_mbs, total_ms);
	(void)write(result_fd, line, (size_t)len);

	free(threads);
	free(args);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <image> <fstype> <part> [threads] "
			"[reads_per_thread]\n",
			argv[0]);
		return 1;
	}

	const char* image = argv[1];
	const char* fstype = argv[2];
	uint32_t part = (uint32_t)atoi(argv[3]);
	int n_threads = (argc > 4) ? atoi(argv[4]) : 4;
	int reads_per_thread = (argc > 5) ? atoi(argv[5]) : 200;

	BackendDef backends[] = {
	    {"raw", ANYFS_OPEN_READONLY},
	    {"aio", ANYFS_OPEN_READONLY | ANYFS_OPEN_AIO},
#ifdef ANYFS_HAS_GIO
	    {"gio-sync", ANYFS_OPEN_READONLY | ANYFS_OPEN_GIO},
	    {"gio-async", ANYFS_OPEN_READONLY | ANYFS_OPEN_GIO_ASYNC},
#endif
	};
	int n_backends = (int)(sizeof(backends) / sizeof(backends[0]));

	printf("Concurrent Benchmark: %s (fs=%s, part=%u)\n", image, fstype,
	       part);
	printf("  %d threads, %d reads/thread (%d total ops)\n\n", n_threads,
	       reads_per_thread, n_threads * reads_per_thread);
	fflush(stdout);

	for (int i = 0; i < n_backends; i++) {
		int pipefd[2];
		if (pipe(pipefd) < 0) {
			perror("pipe");
			return 1;
		}

		pid_t pid = fork();
		if (pid == 0) {
			close(pipefd[0]);
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull >= 0) {
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				close(devnull);
			}
			int rc = run_bench(image, fstype, part, n_threads,
					   reads_per_thread, &backends[i],
					   pipefd[1]);
			close(pipefd[1]);
			_exit(rc == 0 ? 0 : 1);
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
