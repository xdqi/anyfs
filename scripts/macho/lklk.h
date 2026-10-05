/*
 * What liblkl-kernel.dylib exports: the kernel's non-variadic entry points
 * and lkl_elf_glue.c's two helpers, renamed lkl_X -> lklk_X by
 * build_kernel_dylib.sh. Only lkl_macho_shim.c calls these.
 */
#ifndef LKLK_H
#define LKLK_H

struct lkl_host_operations;

int lklk_init(struct lkl_host_operations *ops);
void lklk_cleanup(void);
long lklk_syscall(long no, long *params);
long lklk_sys_halt(void);
int lklk_is_running(void);
int lklk_get_free_irq(const char *user);
void lklk_put_irq(int irq, const char *name);
int lklk_trigger_irq(int irq);
void lklk_glue_set_host(void (*print)(const char *str, int len), void (*panic)(void));
int lklk_start_kernel_str(const char *cmdline);

#endif /* LKLK_H */
