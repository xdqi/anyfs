#ifndef _COMPAT_SYSLOG_H
#define _COMPAT_SYSLOG_H
#include <stdarg.h>
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_NDELAY 0x08
#define LOG_LOCAL5 (21 << 3)
#define LOG_PID 0x01
static inline void openlog(const char* i, int o, int f)
{
	(void)i;
	(void)o;
	(void)f;
}
static inline void closelog(void)
{
}
static inline void syslog(int p, const char* fmt, ...)
{
	(void)p;
	(void)fmt;
}
static inline void vsyslog(int p, const char* fmt, va_list ap)
{
	(void)p;
	(void)fmt;
	(void)ap;
}
#endif
