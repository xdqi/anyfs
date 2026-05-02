// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ipc_lkl.c - ksmbd IPC implementation using LKL raw netlink
 *
 * Replaces mountd/ipc.c (libnl-based) for use with LKL kernel.
 * Provides: ipc_init, ipc_destroy, ipc_process_event, ipc_msg_send,
 *           ipc_msg_alloc, ipc_msg_free
 */

#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lkl.h>
#include <lkl_host.h>

#include <ipc.h>
#include <linux/ksmbd_server.h>
#include <tools.h>
#include <worker.h>

/* Netlink / Generic Netlink constants */
#define NETLINK_GENERIC 16
#define NLM_F_REQUEST 1
#define NLM_F_ACK 4
#define NLMSG_ERROR 2
#define GENL_ID_CTRL 0x10
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_NAME 2
#define CTRL_ATTR_FAMILY_ID 1
#define NLA_ALIGN(len) (((len) + 3) & ~3)

/* Raw netlink headers (matching kernel layout) */
struct nlmsghdr_raw {
	uint32_t nlmsg_len;
	uint16_t nlmsg_type;
	uint16_t nlmsg_flags;
	uint32_t nlmsg_seq;
	uint32_t nlmsg_pid;
};

struct genlmsghdr_raw {
	uint8_t cmd;
	uint8_t version;
	uint16_t reserved;
};

struct nlattr_raw {
	uint16_t nla_len;
	uint16_t nla_type;
};

/* Module state */
static int nlsock = -1;
static int genl_family_id = -1;
static uint32_t nl_seq = 1;

/* ---- Low-level netlink helpers ---- */

static int nl_open(void)
{
	nlsock = lkl_sys_socket(LKL_AF_NETLINK, LKL_SOCK_RAW, NETLINK_GENERIC);
	if (nlsock < 0) {
		pr_err("netlink socket: %s\n", lkl_strerror(nlsock));
		return -1;
	}

	struct {
		uint16_t family;
		uint16_t pad;
		uint32_t pid;
		uint32_t groups;
	} addr = {.family = LKL_AF_NETLINK, .pid = 0, .groups = 0};

	int ret =
	    lkl_sys_bind(nlsock, (struct lkl_sockaddr*)&addr, sizeof(addr));
	if (ret < 0) {
		pr_err("netlink bind: %s\n", lkl_strerror(ret));
		lkl_sys_close(nlsock);
		nlsock = -1;
		return -1;
	}
	return 0;
}

static int nl_send(void* buf, int len)
{
	return lkl_sys_send(nlsock, buf, len, 0);
}

static int nl_recv(void* buf, int len)
{
	return lkl_sys_recv(nlsock, buf, len, 0);
}

static int resolve_genl_family(const char* name)
{
	char buf[512];
	memset(buf, 0, sizeof(buf));

	struct nlmsghdr_raw* nlh = (void*)buf;
	struct genlmsghdr_raw* genl = (void*)(buf + sizeof(*nlh));
	struct nlattr_raw* nla = (void*)(buf + sizeof(*nlh) + sizeof(*genl));

	int name_len = strlen(name) + 1;
	int nla_total = sizeof(*nla) + name_len;

	nlh->nlmsg_len = sizeof(*nlh) + sizeof(*genl) + NLA_ALIGN(nla_total);
	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = nl_seq++;
	nlh->nlmsg_pid = 0;

	genl->cmd = CTRL_CMD_GETFAMILY;
	genl->version = 1;

	nla->nla_len = nla_total;
	nla->nla_type = CTRL_ATTR_FAMILY_NAME;
	memcpy((char*)nla + sizeof(*nla), name, name_len);

	int ret = nl_send(buf, nlh->nlmsg_len);
	if (ret < 0)
		return -1;

	memset(buf, 0, sizeof(buf));
	ret = nl_recv(buf, sizeof(buf));
	if (ret < 0)
		return -1;

	nlh = (void*)buf;
	if (nlh->nlmsg_type == NLMSG_ERROR)
		return -1;

	/* Parse attrs to find CTRL_ATTR_FAMILY_ID */
	genl = (void*)(buf + sizeof(*nlh));
	char* attrs = buf + sizeof(*nlh) + sizeof(*genl);
	int attrs_len = nlh->nlmsg_len - sizeof(*nlh) - sizeof(*genl);

	while (attrs_len >= (int)sizeof(struct nlattr_raw)) {
		nla = (void*)attrs;
		if (nla->nla_type == CTRL_ATTR_FAMILY_ID && nla->nla_len >= 6) {
			uint16_t* fid = (uint16_t*)((char*)nla + sizeof(*nla));
			return *fid;
		}
		int step = NLA_ALIGN(nla->nla_len);
		if (step == 0)
			break;
		attrs += step;
		attrs_len -= step;
	}

	return -1;
}

/* ---- IPC public API ---- */

struct ksmbd_ipc_msg* ipc_msg_alloc(size_t sz)
{
	struct ksmbd_ipc_msg* msg;
	size_t msg_sz = sz + sizeof(struct ksmbd_ipc_msg) + 1;

	if (msg_sz > KSMBD_IPC_MAX_MESSAGE_SIZE)
		return NULL;

	msg = g_try_malloc0(msg_sz);
	if (msg)
		msg->sz = sz;
	return msg;
}

void ipc_msg_free(struct ksmbd_ipc_msg* msg)
{
	g_free(msg);
}

static int ipc_msg_send_internal(struct ksmbd_ipc_msg* msg, int flags)
{
	char buf[KSMBD_IPC_MAX_MESSAGE_SIZE + 256];
	memset(buf, 0, sizeof(buf));

	struct nlmsghdr_raw* nlh = (void*)buf;
	struct genlmsghdr_raw* genl = (void*)(buf + sizeof(*nlh));
	struct nlattr_raw* nla = (void*)(buf + sizeof(*nlh) + sizeof(*genl));

	int nla_total = sizeof(*nla) + msg->sz;

	nlh->nlmsg_len = sizeof(*nlh) + sizeof(*genl) + NLA_ALIGN(nla_total);
	nlh->nlmsg_type = genl_family_id;
	nlh->nlmsg_flags = flags;
	nlh->nlmsg_seq = nl_seq++;
	nlh->nlmsg_pid = 0;

	genl->cmd = msg->type;
	genl->version = KSMBD_GENL_VERSION;

	nla->nla_len = nla_total;
	nla->nla_type = msg->type;
	memcpy((char*)nla + sizeof(*nla), KSMBD_IPC_MSG_PAYLOAD(msg), msg->sz);

	int ret = nl_send(buf, nlh->nlmsg_len);
	if (ret < 0) {
		pr_err("ipc_msg_send failed: %s\n", lkl_strerror(ret));
		return -1;
	}
	return 0;
}

int ipc_msg_send(struct ksmbd_ipc_msg* msg)
{
	return ipc_msg_send_internal(msg, NLM_F_REQUEST);
}

int ipc_process_event(void)
{
	char buf[KSMBD_IPC_MAX_MESSAGE_SIZE + 256];
	struct lkl_pollfd pfd = {.fd = nlsock, .events = LKL_POLLIN};

	int ret = lkl_sys_poll(&pfd, 1, 1000);
	if (ret <= 0)
		return 0; /* timeout or error - not fatal */

	ret = nl_recv(buf, sizeof(buf));
	if (ret <= 0)
		return ret < 0 ? ret : 0;

	struct nlmsghdr_raw* nlh = (void*)buf;

	/* Skip ACK / error messages */
	if (nlh->nlmsg_type == NLMSG_ERROR)
		return 0;

	/* Must be our genl family */
	if (nlh->nlmsg_type != (uint16_t)genl_family_id)
		return 0;

	struct genlmsghdr_raw* genl = (void*)(buf + sizeof(*nlh));

	if (genl->version != KSMBD_GENL_VERSION) {
		pr_err("IPC version mismatch: %d\n", genl->version);
		return 0;
	}

	int cmd = genl->cmd;
	pr_debug("IPC event received: cmd=%d\n", cmd);

	/* Find the attribute matching this command */
	char* attrs = buf + sizeof(*nlh) + sizeof(*genl);
	int attrs_len = nlh->nlmsg_len - sizeof(*nlh) - sizeof(*genl);
	void* payload = NULL;
	int payload_sz = 0;

	while (attrs_len >= (int)sizeof(struct nlattr_raw)) {
		struct nlattr_raw* nla = (void*)attrs;
		if (nla->nla_type == cmd) {
			payload = (char*)nla + sizeof(*nla);
			payload_sz = nla->nla_len - sizeof(*nla);
			break;
		}
		int step = NLA_ALIGN(nla->nla_len);
		if (step == 0)
			break;
		attrs += step;
		attrs_len -= step;
	}

	if (!payload || payload_sz <= 0) {
		pr_err("IPC event %d: no payload attr\n", cmd);
		return 0;
	}

	/* Create IPC message and push to worker */
	struct ksmbd_ipc_msg* event = ipc_msg_alloc(payload_sz);
	if (!event)
		return -ENOMEM;

	event->type = cmd;
	event->sz = payload_sz;
	memcpy(KSMBD_IPC_MSG_PAYLOAD(event), payload, payload_sz);

	pr_debug("Pushing IPC event %d (payload %d bytes) to worker\n", cmd,
		 payload_sz);
	wp_ipc_msg_push(event);
	return 0;
}

static int ipc_ksmbd_starting_up(void)
{
	struct ksmbd_ipc_msg* msg;
	struct ksmbd_startup_request* ev;

	msg = ipc_msg_alloc(sizeof(*ev));
	if (!msg)
		return -ENOMEM;

	ev = KSMBD_IPC_MSG_PAYLOAD(msg);
	msg->type = KSMBD_EVENT_STARTING_UP;

	ev->flags = global_conf.flags;
	ev->signing = global_conf.server_signing;
	ev->tcp_port = global_conf.tcp_port;
	ev->ipc_timeout = global_conf.ipc_timeout;
	ev->deadtime = global_conf.deadtime;
	ev->file_max = global_conf.file_max;
	ev->smb2_max_read = global_conf.smb2_max_read;
	ev->smb2_max_write = global_conf.smb2_max_write;
	ev->smb2_max_trans = global_conf.smb2_max_trans;
	ev->smbd_max_io_size = global_conf.smbd_max_io_size;
	ev->max_connections = global_conf.max_connections;
	ev->share_fake_fscaps = global_conf.share_fake_fscaps;
	ev->smb2_max_credits = global_conf.smb2_max_credits;

	if (global_conf.server_min_protocol)
		strncpy(ev->min_prot, global_conf.server_min_protocol,
			sizeof(ev->min_prot) - 1);
	if (global_conf.server_max_protocol)
		strncpy(ev->max_prot, global_conf.server_max_protocol,
			sizeof(ev->max_prot) - 1);
	if (global_conf.netbios_name)
		strncpy(ev->netbios_name, global_conf.netbios_name,
			sizeof(ev->netbios_name) - 1);
	if (global_conf.server_string)
		strncpy(ev->server_string, global_conf.server_string,
			sizeof(ev->server_string) - 1);
	if (global_conf.work_group)
		strncpy(ev->work_group, global_conf.work_group,
			sizeof(ev->work_group) - 1);

	int ret = ipc_msg_send_internal(msg, NLM_F_REQUEST | NLM_F_ACK);
	if (ret == 0) {
		/* Read ACK from kernel */
		char ack[256];
		int r = nl_recv(ack, sizeof(ack));
		if (r > 0) {
			struct nlmsghdr_raw* a = (void*)ack;
			if (a->nlmsg_type == NLMSG_ERROR) {
				int32_t* errp = (int32_t*)(ack + sizeof(*a));
				if (*errp != 0) {
					pr_err("ksmbd startup rejected: %d\n",
					       *errp);
					ret = *errp;
				}
			}
		}
	}

	ipc_msg_free(msg);
	return ret;
}

void ipc_init(void)
{
	if (nl_open() < 0) {
		pr_err("Failed to open LKL netlink\n");
		return;
	}

	genl_family_id = resolve_genl_family(KSMBD_GENL_NAME);
	if (genl_family_id < 0) {
		pr_err("Failed to resolve %s family\n", KSMBD_GENL_NAME);
		return;
	}
	pr_info("Resolved %s genl family ID: %d\n", KSMBD_GENL_NAME,
		genl_family_id);

	/* Send startup with NLM_F_ACK to confirm */
	/* Override flags for startup only */
	nl_seq--; /* will be re-incremented in ipc_msg_send */
	int ret = ipc_ksmbd_starting_up();
	if (ret < 0) {
		pr_err("ksmbd startup failed: %d\n", ret);
		return;
	}

	/* Give workqueue time to start TCP listener */
	struct __lkl__kernel_timespec ts = {.tv_sec = 1, .tv_nsec = 0};
	lkl_sys_nanosleep(&ts, NULL);

	ksmbd_health_status = KSMBD_HEALTH_RUNNING;
	pr_info("ksmbd IPC initialized, TCP listener should be active\n");
}

void ipc_destroy(void)
{
	if (nlsock >= 0) {
		struct ksmbd_ipc_msg* msg =
		    ipc_msg_alloc(sizeof(struct ksmbd_shutdown_request));
		if (msg) {
			msg->type = KSMBD_EVENT_SHUTTING_DOWN;
			ipc_msg_send(msg);
			ipc_msg_free(msg);
		}
		lkl_sys_close(nlsock);
		nlsock = -1;
	}
	ksmbd_health_status = KSMBD_HEALTH_START;
}
