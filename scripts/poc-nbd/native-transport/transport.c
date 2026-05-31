/*
 * native-transport — minimal N-API addon for the NBD-over-fd PoC.
 *
 * Exports socketpair() -> [fd0, fd1]: a connected AF_UNIX/SOCK_STREAM
 * pair. The parent keeps one fd for the NBD server and lets the child
 * inherit the other (non-CLOEXEC) to use as QEMU's NBD transport.
 *
 * This is the seed of a unified Node-layer native transport abstraction;
 * a Windows loopback-pair helper will be added here later.
 */
#define NAPI_VERSION 8
#include <node_api.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

static napi_value Socketpair(napi_env env, napi_callback_info info)
{
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		napi_throw_error(env, NULL, "socketpair() failed");
		return NULL;
	}
	/* Clear CLOEXEC on the child end (fds[1]) so it survives exec into the
	 * spawned QEMU/lspart child. The parent end (fds[0]) keeps its default
	 * and is never inherited — only the child end is passed via stdio. */
	{
		int fl = fcntl(fds[1], F_GETFD);
		if (fl >= 0)
			fcntl(fds[1], F_SETFD, fl & ~FD_CLOEXEC);
	}

	napi_value arr, a, b;
	napi_create_array_with_length(env, 2, &arr);
	napi_create_int32(env, fds[0], &a);
	napi_create_int32(env, fds[1], &b);
	napi_set_element(env, arr, 0, a);
	napi_set_element(env, arr, 1, b);
	return arr;
}

static napi_value Init(napi_env env, napi_value exports)
{
	napi_value fn;
	napi_create_function(env, "socketpair", NAPI_AUTO_LENGTH, Socketpair,
			     NULL, &fn);
	napi_set_named_property(env, exports, "socketpair", fn);
	return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
