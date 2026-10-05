/*
 * ELF-side glue linked into lkl-kernel.so for the macOS build
 * (docs/superpowers/specs/2026-10-05-lkl-macos-elf2dylib-design.md).
 *
 * The kernel image is ELF code built for the Linux calling convention and
 * converted into liblkl-kernel.dylib by elf2dylib.py. On arm64 the two
 * conventions disagree about variadic functions (Darwin passes variadic
 * arguments on the stack, AAPCS64 in registers), so none may cross the
 * ELF/Mach-O boundary:
 *
 *  - lkl_printf() and lkl_bug(), which the kernel imports, live here and
 *    print through the host's non-variadic print/panic callbacks;
 *  - lkl_start_kernel_str() lets the host start the kernel without making a
 *    variadic call into ELF code.
 *
 * Freestanding: the kernel's own vsnprintf() is hidden by objcopy -G, so this
 * carries a small formatter for what lkl_printf/lkl_bug callers use (%s) and
 * the common integer conversions.
 */
#include <stdarg.h>
#include <stddef.h>

#define GLUE_BUF 512

int lkl_start_kernel(const char *fmt, ...);

static void (*host_print)(const char *str, int len);
static void (*host_panic)(void);

void lkl_glue_set_host(void (*print)(const char *, int), void (*panic)(void))
{
	host_print = print;
	host_panic = panic;
}

int lkl_start_kernel_str(const char *cmdline)
{
	return lkl_start_kernel("%s", cmdline);
}

struct out {
	char *buf;
	int len;
};

static void put(struct out *o, char c)
{
	if (o->len < GLUE_BUF - 1)
		o->buf[o->len++] = c;
}

static void put_str(struct out *o, const char *s)
{
	if (!s)
		s = "(null)";
	while (*s)
		put(o, *s++);
}

static void put_num(struct out *o, unsigned long long v, unsigned int base)
{
	char digits[24];
	int n = 0;

	do {
		digits[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (n)
		put(o, digits[--n]);
}

static int format(char *buf, const char *fmt, va_list ap)
{
	struct out o = { buf, 0 };

	for (; *fmt; fmt++) {
		int is_long = 0;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		while (*++fmt == 'l')
			is_long = 1;
		switch (*fmt) {
		case 's':
			put_str(&o, va_arg(ap, const char *));
			break;
		case 'd':
		case 'i': {
			long long v = is_long ? va_arg(ap, long) : va_arg(ap, int);

			if (v < 0)
				put(&o, '-');
			put_num(&o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10);
			break;
		}
		case 'u':
			put_num(&o, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10);
			break;
		case 'x':
			put_num(&o, is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16);
			break;
		case 'p':
			put_str(&o, "0x");
			put_num(&o, (unsigned long)va_arg(ap, void *), 16);
			break;
		case '%':
			put(&o, '%');
			break;
		case '\0':		/* lone trailing '%': print it and stop */
			put(&o, '%');
			fmt--;
			break;
		default:		/* unsupported conversion: print it verbatim */
			put(&o, '%');
			put(&o, *fmt);
			break;
		}
	}
	buf[o.len] = '\0';
	return o.len;
}

static int emit(const char *fmt, va_list ap)
{
	char buf[GLUE_BUF];
	int n = format(buf, fmt, ap);

	if (host_print)
		host_print(buf, n);
	return n;
}

int lkl_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = emit(fmt, ap);
	va_end(ap);
	return n;
}

void lkl_bug(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	emit(fmt, ap);
	va_end(ap);
	if (host_panic)
		host_panic();
}
