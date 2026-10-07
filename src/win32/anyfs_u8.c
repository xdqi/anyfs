/*
 * anyfs_u8.c — the UTF-8 host layer; see anyfs_u8.h.
 *
 * Must not include anyfs_u8_redirect.h / anyfs_u8_stdio.h: the CRT calls
 * below are the real ones.
 */
#include "anyfs_u8.h"
#include "anyfs_u8_win.h"

#include <errno.h>
#include <string.h>

size_t anyfs_u8_complete_prefix(const char* s, size_t n)
{
	size_t i = n;
	size_t cont = 0;

	while (i > 0 && cont < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
		i--;
		cont++;
	}
	if (i == 0)
		return n; /* only continuation bytes: nothing to wait for */
	unsigned char c = (unsigned char)s[i - 1];
	size_t need = (c >= 0xF0 && c <= 0xF4)	? 4
		      : (c >= 0xE0 && c < 0xF0) ? 3
		      : (c >= 0xC2 && c < 0xE0) ? 2
						: 1;
	if (need > 1 && cont + 1 < need)
		return i - 1;
	return n;
}

#ifdef _WIN32

#include <fcntl.h>

int anyfs_u8_to_u16(const char* s, wchar_t** out)
{
	int n =
	    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	if (n <= 0) {
		errno = EILSEQ;
		return -1;
	}
	wchar_t* w = malloc((size_t)n * sizeof(wchar_t));
	if (!w) {
		errno = ENOMEM;
		return -1;
	}
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
	*out = w;
	return 0;
}

int anyfs_u16_to_u8(const wchar_t* s, char** out)
{
	int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, NULL,
				    0, NULL, NULL);
	if (n <= 0) {
		errno = EILSEQ;
		return -1;
	}
	char* u = malloc((size_t)n);
	if (!u) {
		errno = ENOMEM;
		return -1;
	}
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, u, n, NULL,
			    NULL);
	*out = u;
	return 0;
}

/* Convert path, or return `fail` from the caller with errno set. */
#define WIDE_OR(path, w, fail)                                                 \
	wchar_t* w = NULL;                                                     \
	if (anyfs_u8_to_u16(path, &w) < 0)                                     \
		return fail;

int anyfs_u8_open(const char* path, int flags, ...)
{
	int mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	WIDE_OR(path, w, -1);
	int fd = _wopen(w, flags, mode);
	int e = errno;
	free(w);
	errno = e;
	return fd;
}

FILE* anyfs_u8_fopen(const char* path, const char* mode)
{
	WIDE_OR(path, w, NULL);
	wchar_t* wm = NULL;
	if (anyfs_u8_to_u16(mode, &wm) < 0) {
		free(w);
		return NULL;
	}
	FILE* f = _wfopen(w, wm);
	int e = errno;
	free(w);
	free(wm);
	errno = e;
	return f;
}

int anyfs_u8_stat(const char* path, struct stat* st)
{
	WIDE_OR(path, w, -1);
	struct _stat64 s;
	int r = _wstat64(w, &s);
	int e = errno;
	free(w);
	if (r == 0) {
		memset(st, 0, sizeof(*st));
		st->st_dev = s.st_dev;
		st->st_ino = s.st_ino;
		st->st_mode = s.st_mode;
		st->st_nlink = s.st_nlink;
		st->st_uid = s.st_uid;
		st->st_gid = s.st_gid;
		st->st_rdev = s.st_rdev;
		st->st_size = s.st_size;
		st->st_atime = s.st_atime;
		st->st_mtime = s.st_mtime;
		st->st_ctime = s.st_ctime;
	}
	errno = e;
	return r;
}

int anyfs_u8_access(const char* path, int mode)
{
	WIDE_OR(path, w, -1);
	int r =
	    _waccess(w, mode & ~1); /* no X_OK in msvcrt, as __mingw_access */
	int e = errno;
	free(w);
	errno = e;
	return r;
}

int anyfs_u8_unlink(const char* path)
{
	WIDE_OR(path, w, -1);
	int r = _wunlink(w);
	int e = errno;
	free(w);
	errno = e;
	return r;
}

int anyfs_u8_mkstemp(char* tmpl)
{
	static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	size_t len = strlen(tmpl);
	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return -1;
	}
	unsigned long long seed =
	    GetTickCount64() ^
	    ((unsigned long long)GetCurrentProcessId() << 32) ^
	    (unsigned long long)(uintptr_t)tmpl;
	for (int tries = 0; tries < 100; tries++) {
		for (int i = 0; i < 6; i++) {
			seed = seed * 6364136223846793005ULL +
			       1442695040888963407ULL;
			tmpl[len - 6 + i] = chars[(seed >> 33) % 36];
		}
		int fd = anyfs_u8_open(
		    tmpl, O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0600);
		if (fd >= 0 || errno != EEXIST)
			return fd;
	}
	errno = EEXIST;
	return -1;
}

/* getenv() hands out pointers that stay valid; keep every converted value. */
struct env_entry {
	struct env_entry* next;
	char* name;
	char* value;
};
static struct env_entry* g_env;
static SRWLOCK g_env_lock = SRWLOCK_INIT;

char* anyfs_u8_getenv(const char* name)
{
	wchar_t* wname = NULL;
	if (anyfs_u8_to_u16(name, &wname) < 0)
		return NULL;
	const wchar_t* wv = _wgetenv(wname);
	free(wname);
	char* value = NULL;
	if (!wv || anyfs_u16_to_u8(wv, &value) < 0)
		return NULL;

	AcquireSRWLockExclusive(&g_env_lock);
	for (struct env_entry* e = g_env; e; e = e->next) {
		if (strcmp(e->name, name) == 0 &&
		    strcmp(e->value, value) == 0) {
			ReleaseSRWLockExclusive(&g_env_lock);
			free(value);
			return e->value;
		}
	}
	struct env_entry* e = malloc(sizeof(*e));
	char* n = _strdup(name);
	if (!e || !n) {
		ReleaseSRWLockExclusive(&g_env_lock);
		free(e);
		free(n);
		free(value);
		return NULL;
	}
	e->name = n;
	e->value = value;
	e->next = g_env;
	g_env = e;
	ReleaseSRWLockExclusive(&g_env_lock);
	return value;
}

HANDLE anyfs_u8_create_file(const char* path, DWORD access, DWORD share,
			    DWORD disposition, DWORD attrs)
{
	wchar_t* w = NULL;
	if (anyfs_u8_to_u16(path, &w) < 0) {
		SetLastError(ERROR_INVALID_NAME);
		return INVALID_HANDLE_VALUE;
	}
	HANDLE h =
	    CreateFileW(w, access, share, NULL, disposition, attrs, NULL);
	DWORD e = GetLastError();
	free(w);
	SetLastError(e);
	return h;
}

int anyfs_u8_tmpfile_fd(void)
{
	wchar_t dir[MAX_PATH + 1], path[MAX_PATH + 1];
	DWORD n = GetTempPathW(MAX_PATH + 1, dir);
	if (n == 0 || n > MAX_PATH)
		return -1;
	if (!GetTempFileNameW(dir, L"afs", 0, path))
		return -1;
	/* GetTempFileNameW created the file; reopen it delete-on-close. */
	int fd = _wopen(path, _O_RDWR | _O_BINARY | _O_TEMPORARY);
	if (fd < 0)
		DeleteFileW(path);
	return fd;
}

/* ── Output ─────────────────────────────────────────────────────────── */

/* Held tail of an incomplete UTF-8 sequence, per console stream. */
static struct {
	char b[4];
	size_t n;
} g_held[2];
static SRWLOCK g_out_lock = SRWLOCK_INIT;

/* 0 for stdout, 1 for stderr when it is a console (handle in *h); -1 for
 * everything else, which gets the bytes unchanged. */
static int console_stream(FILE* f, HANDLE* h)
{
	int i = f == stdout ? 0 : f == stderr ? 1 : -1;
	if (i < 0)
		return -1;
	int fd = _fileno(f);
	if (fd < 0)
		return -1;
	HANDLE hh = (HANDLE)_get_osfhandle(fd);
	DWORD mode;
	if (hh == INVALID_HANDLE_VALUE || !GetConsoleMode(hh, &mode))
		return -1;
	*h = hh;
	return i;
}

#define CONSOLE_CHUNK 8192

static void write_console(int i, HANDLE h, const char* s, size_t n)
{
	AcquireSRWLockExclusive(&g_out_lock);
	size_t total = g_held[i].n + n;
	char* buf = malloc(total ? total : 1);
	if (!buf) {
		ReleaseSRWLockExclusive(&g_out_lock);
		return;
	}
	memcpy(buf, g_held[i].b, g_held[i].n);
	memcpy(buf + g_held[i].n, s, n);
	size_t done = anyfs_u8_complete_prefix(buf, total);
	g_held[i].n = total - done;
	memcpy(g_held[i].b, buf + done, g_held[i].n);

	/* Lenient here: an invalid byte prints as U+FFFD instead of
	 * dropping the line. */
	int wn =
	    done ? MultiByteToWideChar(CP_UTF8, 0, buf, (int)done, NULL, 0) : 0;
	wchar_t* w = wn > 0 ? malloc((size_t)wn * sizeof(wchar_t)) : NULL;
	if (w) {
		MultiByteToWideChar(CP_UTF8, 0, buf, (int)done, w, wn);
		int off = 0;
		while (off < wn) {
			int len = wn - off;
			if (len > CONSOLE_CHUNK) {
				len = CONSOLE_CHUNK;
				/* Keep a surrogate pair in one call. */
				if (w[off + len - 1] >= 0xD800 &&
				    w[off + len - 1] <= 0xDBFF)
					len--;
			}
			DWORD written = 0;
			if (!WriteConsoleW(h, w + off, (DWORD)len, &written,
					   NULL) ||
			    written == 0)
				break;
			off += (int)written;
		}
		free(w);
	}
	free(buf);
	ReleaseSRWLockExclusive(&g_out_lock);
}

int anyfs_u8_vfprintf(FILE* f, const char* fmt, va_list ap)
{
	HANDLE h;
	int i = console_stream(f, &h);
	if (i < 0)
		return vfprintf(f, fmt, ap);
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);
	if (n < 0)
		return n;
	char* buf = malloc((size_t)n + 1);
	if (!buf)
		return -1;
	vsnprintf(buf, (size_t)n + 1, fmt, ap);
	fflush(f);
	write_console(i, h, buf, (size_t)n);
	free(buf);
	return n;
}

int anyfs_u8_fprintf(FILE* f, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = anyfs_u8_vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}

int anyfs_u8_vprintf(const char* fmt, va_list ap)
{
	return anyfs_u8_vfprintf(stdout, fmt, ap);
}

int anyfs_u8_printf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = anyfs_u8_vfprintf(stdout, fmt, ap);
	va_end(ap);
	return n;
}

size_t anyfs_u8_fwrite(const void* p, size_t size, size_t n, FILE* f)
{
	HANDLE h;
	int i = console_stream(f, &h);
	if (i < 0)
		return fwrite(p, size, n, f);
	fflush(f);
	write_console(i, h, p, size * n);
	return n;
}

int anyfs_u8_fputs(const char* s, FILE* f)
{
	HANDLE h;
	int i = console_stream(f, &h);
	if (i < 0)
		return fputs(s, f);
	fflush(f);
	write_console(i, h, s, strlen(s));
	return 0;
}

int anyfs_u8_puts(const char* s)
{
	if (anyfs_u8_fputs(s, stdout) < 0)
		return EOF;
	return anyfs_u8_fputc('\n', stdout) == EOF ? EOF : 0;
}

int anyfs_u8_fputc(int c, FILE* f)
{
	HANDLE h;
	int i = console_stream(f, &h);
	if (i < 0)
		return fputc(c, f);
	char b = (char)c;
	fflush(f);
	write_console(i, h, &b, 1);
	return (unsigned char)c;
}

int anyfs_u8_putchar(int c)
{
	return anyfs_u8_fputc(c, stdout);
}

void anyfs_u8_perror(const char* s)
{
	const char* msg = strerror(errno);
	if (s && *s)
		anyfs_u8_fprintf(stderr, "%s: %s\n", s, msg);
	else
		anyfs_u8_fprintf(stderr, "%s\n", msg);
}

#endif /* _WIN32 */
