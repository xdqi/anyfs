/* Native unit test for lkl_elf_glue.c: formatter, print/panic routing and
 * the non-variadic start entry point. Built by test_glue.sh. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
	char big[2000];
	int n;

	lkl_glue_set_host(fake_print, fake_panic);

	printed_len = 0;
	n = lkl_printf("%s: unbalanced put\n", "lkl_cpu_put");
	EXPECT(strcmp(printed, "lkl_cpu_put: unbalanced put\n") == 0);
	EXPECT(n == (int)strlen(printed));

	printed_len = 0;
	lkl_printf("d=%d i=%i u=%u x=%x ld=%ld lu=%lu lx=%lx p=%p %% end", -5, 7,
		   3000000000u, 0xbeef, -1234567890123L, 9876543210UL,
		   0xdeadbeefcafeUL, (void *)0x1000);
	EXPECT(strcmp(printed, "d=-5 i=7 u=3000000000 x=beef ld=-1234567890123 "
		   "lu=9876543210 lx=deadbeefcafe p=0x1000 % end") == 0);

	printed_len = 0;
	lkl_printf("%s|%s|%q|", (char *)0, "");
	EXPECT(strcmp(printed, "(null)||%q|") == 0);

	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	printed_len = 0;
	n = lkl_printf("%s", big);
	EXPECT(n == 511 && printed_len == 511);	/* truncated, not overrun */

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
