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
 * The print and panic callbacks are captured once, when lkl_init() passes
 * them to lkl_glue_set_host(). The native host library reads lkl_host_ops on
 * every call instead, so changing lkl_host_ops.print after lkl_init() has no
 * effect here.
 *
 * Freestanding: the kernel's own vsnprintf() is hidden by objcopy -G, so this
 * carries a small formatter for %s %d %i %u %x %X %p %c and %%. It skips
 * flags, width and precision, and reads z, t, j, l and ll as 64-bit. On any
 * other conversion it prints the rest of the format as is and reads no more
 * arguments, so it never reads one of the wrong type.
 */
#include <stdarg.h>

_Static_assert(sizeof(long) == sizeof(long long), "lkl_elf_glue assumes LP64");

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

static void put_num(struct out *o, unsigned long long v, unsigned int base, int upper)
{
	const char *digit = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char digits[24];
	int n = 0;

	do {
		digits[n++] = digit[v % base];
		v /= base;
	} while (v);
	while (n)
		put(o, digits[--n]);
}

/* Skips a width or precision: digits, or a '*', which takes an int argument. */
static const char *skip_count(const char *p, int *stars)
{
	if (*p == '*') {
		++*stars;
		return p + 1;
	}
	while (*p >= '0' && *p <= '9')
		p++;
	return p;
}

static int format(char *buf, const char *fmt, va_list ap)
{
	struct out o = { buf, 0 };

	for (; *fmt; fmt++) {
		const char *p = fmt + 1;
		int stars = 0, wide = 0;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		if (*p == '%') {
			put(&o, '%');
			fmt = p;
			continue;
		}
		while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')
			p++;
		p = skip_count(p, &stars);
		if (*p == '.')
			p = skip_count(p + 1, &stars);
		if (*p == 'h') {
			p += p[1] == 'h' ? 2 : 1;
		} else if (*p == 'l') {
			p += p[1] == 'l' ? 2 : 1;
			wide = 1;
		} else if (*p == 'z' || *p == 't' || *p == 'j') {
			p++;
			wide = 1;
		}
		switch (*p) {
		case 's': case 'd': case 'i': case 'u': case 'x': case 'X':
		case 'p': case 'c':
			break;
		default:
			/* Unsupported, or cut short by the end of fmt: the rest goes
			 * out as is, and no further argument is read. */
			put_str(&o, fmt);
			goto done;
		}
		while (stars--)
			(void)va_arg(ap, int);
		fmt = p;
		switch (*p) {
		case 's':
			put_str(&o, va_arg(ap, const char *));
			break;
		case 'c':
			put(&o, (char)va_arg(ap, int));
			break;
		case 'p':
			put_str(&o, "0x");
			put_num(&o, (unsigned long)va_arg(ap, void *), 16, 0);
			break;
		case 'd':
		case 'i': {
			long long v = wide ? va_arg(ap, long long) : va_arg(ap, int);

			if (v < 0)
				put(&o, '-');
			put_num(&o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10, 0);
			break;
		}
		default: {		/* u, x, X */
			unsigned long long v = wide ? va_arg(ap, unsigned long long)
						    : va_arg(ap, unsigned int);

			put_num(&o, v, *p == 'u' ? 10 : 16, *p == 'X');
			break;
		}
		}
	}
done:
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
	else
		__builtin_trap();	/* no panic hook: lkl_bug must not return */
}
