/*
 * Stubs for linking QEMU libblock.a into anyfs-reader.
 * These symbols are pulled in by libio.a and libqom.a but are not needed
 * for our use case (just blk_pread/blk_pwrite on local images).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Trace DSTATE stubs - all disabled */
uint16_t _TRACE_OBJECT_CLASS_DYNAMIC_CAST_ASSERT_DSTATE;
uint16_t _TRACE_OBJECT_DYNAMIC_CAST_ASSERT_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_FILE_NEW_FD_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_FILE_NEW_PATH_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_ACCEPT_COMPLETE_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_ACCEPT_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_ACCEPT_FAIL_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_CONNECT_ASYNC_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_CONNECT_COMPLETE_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_CONNECT_FAIL_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_CONNECT_SYNC_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_DGRAM_ASYNC_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_DGRAM_COMPLETE_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_DGRAM_FAIL_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_DGRAM_SYNC_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_LISTEN_ASYNC_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_LISTEN_COMPLETE_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_LISTEN_FAIL_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_LISTEN_SYNC_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_NEW_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_SOCKET_NEW_FD_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_BYE_CANCEL_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_BYE_FAIL_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_BYE_PENDING_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_BYE_START_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_CREDENTIALS_ALLOW_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_CREDENTIALS_DENY_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_HANDSHAKE_CANCEL_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_HANDSHAKE_COMPLETE_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_HANDSHAKE_FAIL_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_HANDSHAKE_PENDING_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_HANDSHAKE_START_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_NEW_CLIENT_DSTATE;
uint16_t _TRACE_QIO_CHANNEL_TLS_NEW_SERVER_DSTATE;
uint16_t _TRACE_QIO_TASK_COMPLETE_DSTATE;
uint16_t _TRACE_QIO_TASK_NEW_DSTATE;
uint16_t _TRACE_QIO_TASK_THREAD_EXIT_DSTATE;
uint16_t _TRACE_QIO_TASK_THREAD_RESULT_DSTATE;
uint16_t _TRACE_QIO_TASK_THREAD_RUN_DSTATE;
uint16_t _TRACE_QIO_TASK_THREAD_SOURCE_ATTACH_DSTATE;
uint16_t _TRACE_QIO_TASK_THREAD_SOURCE_CANCEL_DSTATE;
uint16_t _TRACE_QIO_TASK_THREAD_START_DSTATE;

/* QOM stubs */
const char* const ObjectType_lookup[] = {NULL};

void qapi_free_ObjectOptions(void* obj)
{
	(void)obj;
}
void* visit_type_ObjectOptions(void* v, const char* name, void** obj,
			       void* errp)
{
	(void)v;
	(void)name;
	(void)obj;
	(void)errp;
	return NULL;
}

/* QAPI visitor stubs */
void* string_input_visitor_new(const char* str)
{
	(void)str;
	return NULL;
}
void* string_output_visitor_new(int human)
{
	(void)human;
	return NULL;
}
void* visitor_forward_field(void* v, const char* n, const char* fn)
{
	(void)v;
	(void)n;
	(void)fn;
	return NULL;
}

/* Crypto stubs */
int qcrypto_random_bytes(void* buf, size_t len, void* errp)
{
	/* Use /dev/urandom for actual randomness if needed */
	memset(buf, 0, len);
	(void)errp;
	return 0;
}

/* UUID - now provided by libqemuutil */

/* bitmap - now provided by libqemuutil */
