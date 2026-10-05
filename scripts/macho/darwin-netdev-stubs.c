/* Darwin stubs for the Linux-only netdev backends.
 *
 * lib/config.c dispatches on an interface-type string and references
 * lkl_netdev_{tap,macvtap,raw}_create() unconditionally, the same way it already
 * references the dpdk and vde backends that are normally not built. Those three
 * are Linux-specific -- tap needs <linux/if_tun.h>, raw needs AF_PACKET, macvtap
 * needs both -- so on macOS they can only ever fail.
 *
 * build_host_lib.sh compiles this in place of virtio_net_tap.c and
 * virtio_net_raw.c. anyfs builds LKL with CONFIG_NET on (ksmbd and nfsd need
 * it), but these are reached only if a netdev of type tap or raw is
 * configured, which anyfs does not do.
 */
#include <lkl_host.h>

static struct lkl_netdev *unsupported(const char *what)
{
	lkl_printf("lkl: netdev backend \"%s\" is not available on macOS\n", what);
	return NULL;
}

struct lkl_netdev *lkl_netdev_tap_create(const char *ifname, int offload)
{
	(void)ifname; (void)offload;
	return unsupported("tap");
}

/* macvtap already has a stub of its own once LKL_HOST_CONFIG_VIRTIO_NET_MACVTAP
 * is off, so only tap and raw need one here. */

struct lkl_netdev *lkl_netdev_raw_create(const char *ifname)
{
	(void)ifname;
	return unsupported("raw");
}
