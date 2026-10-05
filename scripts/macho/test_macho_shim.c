/* Native unit test for lkl_macho_shim.c against fake lklk_* entry points.
 * Built by test_glue.sh. */
#include <stdio.h>
#include <string.h>

#include <lkl_host.h>

#include "lklk.h"

static char trace[256];
static struct lkl_host_operations *init_ops;
static void (*got_print)(const char *, int);
static void (*got_panic)(void);
static char started[8192];
static int start_calls, put_irq_seen = -1, failures;

#define EXPECT(cond)							\
	do {								\
		if (!(cond)) {						\
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;					\
		}							\
	} while (0)

static void note(const char *what)
{
	strncat(trace, what, sizeof(trace) - strlen(trace) - 1);
}

void lklk_glue_set_host(void (*print)(const char *, int), void (*panic)(void))
{
	note("set_host ");
	got_print = print;
	got_panic = panic;
}

int lklk_init(struct lkl_host_operations *ops)
{
	note("init ");
	init_ops = ops;
	return 7;
}

int lklk_start_kernel_str(const char *cmdline)
{
	start_calls++;
	snprintf(started, sizeof(started), "%s", cmdline);
	return 3;
}

void lklk_cleanup(void) { note("cleanup "); }
long lklk_syscall(long no, long *params) { return no * 100 + params[0]; }
long lklk_sys_halt(void) { return 11; }
int lklk_is_running(void) { return 1; }
int lklk_get_free_irq(const char *user) { return (int)strlen(user); }
void lklk_put_irq(int irq, const char *name) { (void)name; put_irq_seen = irq; }
int lklk_trigger_irq(int irq) { return irq + 1; }

static void fake_print(const char *s, int len) { (void)s; (void)len; }
static void fake_panic(void) { }

int main(void)
{
	struct lkl_host_operations ops = { .print = fake_print, .panic = fake_panic };
	long params[6] = { 5 };
	char big[5000];

	EXPECT(lkl_init(&ops) == 7);
	EXPECT(strcmp(trace, "set_host init ") == 0);	/* glue first */
	EXPECT(init_ops == &ops && got_print == fake_print && got_panic == fake_panic);

	EXPECT(lkl_start_kernel("mem=%dM %s", 64, "loglevel=4") == 3);
	EXPECT(strcmp(started, "mem=64M loglevel=4") == 0);

	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	start_calls = 0;
	EXPECT(lkl_start_kernel("%s", big) == -LKL_E2BIG);
	EXPECT(start_calls == 0);
	big[4096] = '\0';				/* 4096 + NUL does not fit */
	EXPECT(lkl_start_kernel("%s", big) == -LKL_E2BIG);
	EXPECT(start_calls == 0);
	big[4095] = '\0';				/* 4095 + NUL still fits */
	EXPECT(lkl_start_kernel("%s", big) == 3 && start_calls == 1);
	EXPECT(strlen(started) == 4095);

	/* vsnprintf fails: the C locale cannot encode U+20AC. */
	start_calls = 0;
	EXPECT(lkl_start_kernel("%ls", L"\x20ac") == -LKL_EINVAL);
	EXPECT(start_calls == 0);

	EXPECT(lkl_syscall(2, params) == 205);
	EXPECT(lkl_sys_halt() == 11);
	EXPECT(lkl_is_running() == 1);
	EXPECT(lkl_get_free_irq("virtio") == 6);
	lkl_put_irq(9, "virtio");
	EXPECT(put_irq_seen == 9);
	EXPECT(lkl_trigger_irq(4) == 5);
	lkl_cleanup();
	EXPECT(strstr(trace, "cleanup") != NULL);

	printf(failures ? "FAILED test_macho_shim\n" : "PASS test_macho_shim\n");
	return failures != 0;
}
