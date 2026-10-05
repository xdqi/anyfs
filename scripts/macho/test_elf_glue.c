/* Native unit test for lkl_elf_glue.c: formatter, print/panic routing and
 * the non-variadic start entry point. Built by test_glue.sh. */
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

void lkl_glue_set_host(void (*print)(const char *, int), void (*panic)(void));
int lkl_start_kernel_str(const char *cmdline);
int lkl_printf(const char *fmt, ...);
void lkl_bug(const char *fmt, ...);

static char printed[1024];
static int printed_len, panics;
static const char *start_fmt, *start_arg;
static int failures;

#define EXPECT(cond)							\
	do {								\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;					\
		}							\
	} while (0)

/* lkl_printf(...) must print exactly want and return its length. */
#define EXPECT_PRINTS(want, ...)					\
	do {								\
		int n_;							\
									\
		printed_len = 0;					\
		printed[0] = '\0';					\
		n_ = lkl_printf(__VA_ARGS__);				\
		if (strcmp(printed, want) || n_ != (int)strlen(want)) {	\
			printf("FAIL %s:%d: printed \"%s\" (returned %d), expected \"%s\"\n", \
			       __FILE__, __LINE__, printed, n_, want);	\
			failures++;					\
		}							\
	} while (0)

static void fake_print(const char *s, int len)
{
	memcpy(printed + printed_len, s, len);
	printed_len += len;
	printed[printed_len] = '\0';
}

static void fake_panic(void)
{
	panics++;
}

/* Stands in for the kernel's lkl_start_kernel(). */
int lkl_start_kernel(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	start_fmt = fmt;
	start_arg = va_arg(ap, const char *);
	va_end(ap);
	return 42;
}

/* lkl_bug() without a panic hook must not return: it traps. */
static void expect_bug_traps_without_panic(void)
{
	int status = 0;
	pid_t pid = fork();

	if (pid == 0) {
		struct rlimit no_core = { 0, 0 };

		setrlimit(RLIMIT_CORE, &no_core);
		lkl_glue_set_host(fake_print, NULL);
		lkl_bug("no panic hook\n");
		_exit(0);
	}
	EXPECT(pid > 0 && waitpid(pid, &status, 0) == pid);
	EXPECT(WIFSIGNALED(status) &&
	       (WTERMSIG(status) == SIGILL || WTERMSIG(status) == SIGTRAP));
}

int main(void)
{
	char big[2000];
	int n;

	/* First: the glue's state is static, and nothing is set yet. */
	EXPECT(lkl_printf("%s=%d\n", "early", 5) == 8);
	EXPECT(printed_len == 0);

	expect_bug_traps_without_panic();

	lkl_glue_set_host(fake_print, fake_panic);

	printed_len = 0;
	n = lkl_printf("%s: unbalanced put\n", "lkl_cpu_put");
	EXPECT(strcmp(printed, "lkl_cpu_put: unbalanced put\n") == 0);
	EXPECT(n == (int)strlen(printed));

	EXPECT_PRINTS("d=-5 i=7 u=3000000000 x=beef ld=-1234567890123 "
		      "lu=9876543210 lx=deadbeefcafe p=0x1000 % end",
		      "d=%d i=%i u=%u x=%x ld=%ld lu=%lu lx=%lx p=%p %% end", -5, 7,
		      3000000000u, 0xbeef, -1234567890123L, 9876543210UL,
		      0xdeadbeefcafeUL, (void *)0x1000);
	EXPECT_PRINTS("(null)||%q|", "%s|%s|%q|", (char *)0, "");

	/* Limits and length modifiers. */
	EXPECT_PRINTS("-9223372036854775808|-2147483648|0|2147483647",
		      "%ld|%d|%d|%d", LONG_MIN, INT_MIN, 0, INT_MAX);
	EXPECT_PRINTS("-9223372036854775808|18446744073709551615|ffffffffffffffff",
		      "%lld|%llu|%llx", LLONG_MIN, ULLONG_MAX, ULLONG_MAX);
	EXPECT_PRINTS("4294967295|0|p=0x0", "%u|%x|p=%p", UINT_MAX, 0u, (void *)0);
	EXPECT_PRINTS("18446744073709551615|abc|-3|-9223372036854775808|end",
		      "%zu|%zx|%td|%jd|%s", SIZE_MAX, (size_t)0xabc, (ptrdiff_t)-3,
		      INTMAX_MIN, "end");
	EXPECT_PRINTS("5|str", "%zu|%s", (size_t)5, "str");

	/* Flags, width and precision are skipped; a '*' takes an int. */
	EXPECT_PRINTS("beef|42|abcdef|5|ff", "%08lx|%-5d|%.3s|%+d|%#x",
		      0xbeefUL, 42, "abcdef", 5, 0xff);
	EXPECT_PRINTS("7|after", "%*d|%s", 10, 7, "after");
	EXPECT_PRINTS("xyz|9|ok", "%.*s|%-*.*d|%s", 2, "xyz", 4, 2, 9, "ok");

	/* Other conversions; h and hh read an int. */
	EXPECT_PRINTS("A|BEEF|-3|200|%", "%c|%X|%hd|%hhu|%%", 'A', 0xbeef,
		      (short)-3, (unsigned char)200);

	/* An unsupported conversion ends formatting: the rest is printed as is
	 * and no argument after it is read. */
	EXPECT_PRINTS("%q|%s", "%q|%s", "x");
	EXPECT_PRINTS("1 %-5lq|%s %d", "%d %-5lq|%s %d", 1, "x", 2);
	EXPECT_PRINTS("n=%n|%s", "n=%n|%s", &n, "x");
	EXPECT_PRINTS("%5%|%s", "%5%|%s", "x");

	/* A conversion cut short by the end of fmt is printed as is. */
	EXPECT_PRINTS("100%", "100%");
	EXPECT_PRINTS("5%", "%d%", 5);
	EXPECT_PRINTS("x=%l", "x=%l");
	EXPECT_PRINTS("x=%-0", "x=%-0");

	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	printed_len = 0;
	n = lkl_printf("%s", big);
	EXPECT(n == 511 && printed_len == 511);	/* truncated, not overrun */
	printed_len = 0;
	n = lkl_printf("%s%q%s", big, "x");
	EXPECT(n == 511 && printed_len == 511);

	printed_len = 0;
	panics = 0;
	lkl_bug("bad count while changing owner\n");
	EXPECT(strcmp(printed, "bad count while changing owner\n") == 0);
	EXPECT(panics == 1);

	EXPECT(lkl_start_kernel_str("mem=64M loglevel=4") == 42);
	EXPECT(start_fmt && strcmp(start_fmt, "%s") == 0);
	EXPECT(start_arg && strcmp(start_arg, "mem=64M loglevel=4") == 0);

	printf(failures ? "FAILED test_elf_glue\n" : "PASS test_elf_glue\n");
	return failures != 0;
}
