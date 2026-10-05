/* Darwin host profile for the LKL host layer.
 *
 * build_host_lib.sh puts this directory first on the include path, so it
 * shadows the lkl_autoconf.h that the Linux build tree generated for a Linux
 * host. It is what a Darwin branch in tools/lkl/Makefile.autoconf would emit,
 * next to the existing posix_host / nt64_host / bsd_host profiles.
 *
 * Off, deliberately:
 *   VFIO_PCI            - no VFIO on macOS; posix-host.c would reference
 *                         vfio_pci_ops
 *   VIRTIO_NET_MACVTAP  - needs <linux/if_tun.h>
 *   FUSE                - lklfuse targets libfuse; macFUSE/FUSE-T is a separate
 *                         question and anyfs browses in-app
 *
 * The netdev backends config.c references unconditionally (tap/macvtap/raw) are
 * satisfied by darwin-netdev-stubs.c.
 */
#define LKL_HOST_CONFIG_POSIX y
#define LKL_HOST_CONFIG_VIRTIO_NET y
#define LKL_HOST_CONFIG_VIRTIO_NET_FD y
