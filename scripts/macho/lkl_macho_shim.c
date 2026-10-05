/*
 * The public LKL API for the macOS build. The kernel is liblkl-kernel.dylib,
 * converted from the standard ELF build by scripts/macho/build_kernel_dylib.sh,
 * which exports its entry points as lklk_* (lklk.h). This file gives them
 * their usual lkl_* names, so the LKL host library and anyfs need no changes,
 * and keeps variadic calls on the Mach-O side: on arm64, Darwin passes
 * variadic arguments on the stack while the ELF code expects them in
 * registers (see lkl_elf_glue.c).
 */
#include <stdarg.h>
#include <stdio.h>

#include <lkl_host.h>

#include "lklk.h"

/* COMMAND_LINE_SIZE in arch/lkl/include/asm/setup.h */
#define LKL_CMDLINE_MAX 4096

int lkl_init(struct lkl_host_operations *ops)
{
	lklk_glue_set_host(ops->print, ops->panic);
	return lklk_init(ops);
}

int lkl_start_kernel(const char *fmt, ...)
{
	char cmdline[LKL_CMDLINE_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(cmdline, sizeof(cmdline), fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)sizeof(cmdline))
		return -LKL_E2BIG;
	return lklk_start_kernel_str(cmdline);
}

void lkl_cleanup(void)
{
	lklk_cleanup();
}

long lkl_syscall(long no, long *params)
{
	return lklk_syscall(no, params);
}

long lkl_sys_halt(void)
{
	return lklk_sys_halt();
}

int lkl_is_running(void)
{
	return lklk_is_running();
}

int lkl_get_free_irq(const char *user)
{
	return lklk_get_free_irq(user);
}

void lkl_put_irq(int irq, const char *name)
{
	lklk_put_irq(irq, name);
}

int lkl_trigger_irq(int irq)
{
	return lklk_trigger_irq(irq);
}
