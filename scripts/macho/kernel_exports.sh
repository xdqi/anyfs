# The kernel entry points that cross the ELF/Mach-O boundary, sourced by
# build_kernel_dylib.sh and build_host_lib.sh. build_kernel_dylib.sh exports
# each lkl_X of lkl-kernel.so as _lklk_X of liblkl-kernel.dylib, and
# build_host_lib.sh requires liblkl-host.a to import exactly those _lklk_X.
# lklk.h declares them as lklk_X, lkl_macho_shim.c calls them, and
# test_lklk_dlsym.c forwards them on Linux: keep all three in step with this
# list.
#
# The ABI boundary (the spec's "ABI boundary audit"). Darwin arm64 departs
# from AAPCS64 for variadic calls, arguments narrower than 32 bits,
# stack-passed arguments and some by-value aggregates. Every entry point
# below, every member of struct lkl_host_operations, and every callback the
# kernel hands the host to call back into ELF code (thread_create's entry
# point, timer_alloc's fn, tls_alloc's destructor, jmp_buf_set's f) takes at
# most 8 int/long/enum/pointer arguments, none narrower than 32 bits and
# nothing by value, and none is variadic: lkl_start_kernel, lkl_printf and
# lkl_bug are deliberately absent (see lkl_elf_glue.c). Return values are
# void, int, long, unsigned long[ long] or pointers: nothing narrower than 32
# bits, no struct, float or union. char signedness differs (unsigned on
# Linux arm64, signed on Darwin), but chars cross only behind pointers.
# jmp_buf_set/jmp_buf_longjmp run Darwin setjmp/longjmp across kernel frames.
# That is safe because both conventions have the same callee-saved registers
# (arm64 x19-x28, d8-d15, fp, lr; x86_64 rbx, rbp, r12-r15), longjmp only
# restores them and unwinds nothing, and the kernel never touches x18.
# lkl_host_operations.pci_ops, a struct lkl_dev_pci_ops table, stays NULL on
# Darwin because VFIO is off; enabling VFIO brings that table into the
# boundary. Keep all of this true when adding to this list or to
# lkl_host_operations.
# shellcheck shell=bash

# shellcheck disable=SC2034  # used by the scripts that source this file
KERNEL_EXPORTS=(
    lkl_init lkl_cleanup lkl_syscall lkl_sys_halt lkl_is_running
    lkl_get_free_irq lkl_put_irq lkl_trigger_irq
    lkl_glue_set_host lkl_start_kernel_str
)
