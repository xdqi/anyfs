// SPDX-License-Identifier: GPL-2.0-or-later
#include "server_common.h"

#include <lkl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

volatile sig_atomic_t anyfs_server_running = 1;

static void handle_stop(int sig)
{
	(void)sig;
	anyfs_server_running = 0;
}

#ifdef _WIN32
/*
 * A crash on Windows ends the process without a word (no core dump, and the
 * release binaries are stripped). Print the exception and the faulting
 * stack as module+offset pairs, which map back to a build of the same
 * commit, then hand over to the previous filter.
 */
static LPTOP_LEVEL_EXCEPTION_FILTER prev_filter;

static void print_frame(const char* what, DWORD64 addr)
{
	HMODULE mod = NULL;
	char path[MAX_PATH] = "?";

	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)(uintptr_t)addr, &mod) &&
	    mod)
		GetModuleFileNameA(mod, path, sizeof(path));
	const char* name = strrchr(path, '\\');
	fprintf(stderr, "  %s %s+0x%llx\n", what, name ? name + 1 : path,
		(unsigned long long)(addr - (uintptr_t)mod));
}

static LONG WINAPI report_crash(EXCEPTION_POINTERS* ep)
{
	EXCEPTION_RECORD* er = ep->ExceptionRecord;

	fprintf(stderr, "fatal: exception 0x%08lx", er->ExceptionCode);
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
	    er->NumberParameters >= 2)
		fprintf(stderr, " (%s of 0x%llx)",
			er->ExceptionInformation[0] == 0   ? "read"
			: er->ExceptionInformation[0] == 1 ? "write"
							   : "execute",
			(unsigned long long)er->ExceptionInformation[1]);
	fprintf(stderr, " in thread %lu\n", GetCurrentThreadId());
#if defined(__x86_64__) || defined(_M_X64)
	CONTEXT ctx = *ep->ContextRecord;
	for (int i = 0; i < 32 && ctx.Rip; i++) {
		print_frame(i ? "from" : "at", ctx.Rip);
		DWORD64 base;
		PRUNTIME_FUNCTION fn =
		    RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
		if (!fn) { /* a leaf: the return address is on top */
			ctx.Rip = *(DWORD64*)(uintptr_t)ctx.Rsp;
			ctx.Rsp += 8;
			continue;
		}
		PVOID handler_data;
		DWORD64 frame;
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx,
				 &handler_data, &frame, NULL);
	}
#else
	print_frame("at", (uintptr_t)er->ExceptionAddress);
#endif
	fflush(stderr);
	return prev_filter ? prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

void anyfs_server_install_signals(void)
{
	setbuf(stdout, NULL);
	signal(SIGINT, handle_stop);
	signal(SIGTERM, handle_stop);
#ifdef _WIN32
	prev_filter = SetUnhandledExceptionFilter(report_crash);
#endif
}

int anyfs_server_boot(const AnyfsKernelOpts* opts)
{
	int ret = anyfs_kernel_init(opts);
	if (ret)
		return ret;
	/* lo is auto-up after boot, but the call is idempotent and
	 * documents intent. */
	lkl_if_up(1);
	return 0;
}

int anyfs_server_resolve_shares(char* const* specs, int n_specs,
				AnyfsSession** disks, int n_disks,
				uint32_t enter_flags, AnyfsShareEntry* out,
				int max_out)
{
	int n = 0;

	for (int si = 0; si < n_specs; si++) {
		if (n >= max_out) {
			fprintf(stderr, "error: too many shares (max %d)\n",
				max_out);
			return -1;
		}
		AnyfsShareEntry* e = &out[n];
		if (anyfs_share_resolve(specs[si], disks, n_disks, enter_flags,
					e->name, sizeof(e->name), e->lkl_path,
					sizeof(e->lkl_path)) < 0)
			return -1;
		n++;
	}
	return n;
}

void anyfs_server_shutdown(AnyfsSession** disks, int n_disks)
{
	for (int i = 0; i < n_disks; i++) {
		if (disks[i])
			anyfs_session_close(disks[i]);
	}
	anyfs_kernel_halt();
}
