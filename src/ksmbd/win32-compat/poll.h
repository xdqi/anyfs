#ifndef _COMPAT_POLL_H
#define _COMPAT_POLL_H
/* Minimal poll.h for ksmbd-tools on Win32. */
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#ifndef _STRUCT_POLLFD_DEFINED
struct pollfd {
	int fd;
	short events;
	short revents;
};
#define _STRUCT_POLLFD_DEFINED
#endif
static inline int poll(struct pollfd* fds, unsigned long nfds, int timeout)
{
	(void)fds;
	(void)nfds;
	(void)timeout;
	return -1;
}
#endif
