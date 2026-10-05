/*
 * lklk.h on Linux, for test_kernel_so.sh: each lklk_X calls lkl_X of the ELF
 * lkl-kernel.so named by $LKLK_KERNEL_SO, which is what the export map of
 * liblkl-kernel.dylib does on macOS. The lookup goes through dlsym() on that
 * library's own handle, because the test program also defines every lkl_X
 * (lkl_macho_shim.c): a link-time alias or a global lookup would find the
 * shim's lkl_X and recurse. lkl-kernel.so is linked with -Bsymbolic, so the
 * kernel's own calls to lkl_printf() and lkl_bug() stay inside it and reach
 * lkl_elf_glue.c, not the host library's versions.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "lklk.h"

static void *kernel;

__attribute__((constructor)) static void open_kernel(void)
{
	const char *path = getenv("LKLK_KERNEL_SO");

	if (!path) {
		fprintf(stderr, "test_lklk_dlsym: set LKLK_KERNEL_SO to lkl-kernel.so\n");
		exit(1);
	}
	kernel = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!kernel) {
		fprintf(stderr, "test_lklk_dlsym: %s\n", dlerror());
		exit(1);
	}
}

static void *kernel_sym(const char *name)
{
	void *sym = dlsym(kernel, name);

	if (!sym) {
		fprintf(stderr, "test_lklk_dlsym: %s: no %s\n",
			getenv("LKLK_KERNEL_SO"), name);
		exit(1);
	}
	return sym;
}

/* KERNEL(X): lkl_X in lkl-kernel.so, with the type of lklk_X. */
#define KERNEL(name) ((__typeof__(&lklk_##name))kernel_sym("lkl_" #name))

int lklk_init(struct lkl_host_operations *ops)
{
	return KERNEL(init)(ops);
}

void lklk_cleanup(void)
{
	KERNEL(cleanup)();
}

long lklk_syscall(long no, long *params)
{
	return KERNEL(syscall)(no, params);
}

long lklk_sys_halt(void)
{
	return KERNEL(sys_halt)();
}

int lklk_is_running(void)
{
	return KERNEL(is_running)();
}

int lklk_get_free_irq(const char *user)
{
	return KERNEL(get_free_irq)(user);
}

void lklk_put_irq(int irq, const char *name)
{
	KERNEL(put_irq)(irq, name);
}

int lklk_trigger_irq(int irq)
{
	return KERNEL(trigger_irq)(irq);
}

void lklk_glue_set_host(void (*print)(const char *str, int len), void (*panic)(void))
{
	KERNEL(glue_set_host)(print, panic);
}

int lklk_start_kernel_str(const char *cmdline)
{
	return KERNEL(start_kernel_str)(cmdline);
}
