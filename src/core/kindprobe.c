/*
 * kindprobe.c — classify a partition by superblock magic + libblkid.
 *
 * One pread of the first 64KB is enough for the three load-bearing
 * v1 magic checks. Anything that matches none of them lands in KIND_FS
 * and the kernel figures out the filesystem at mount time.
 *
 * v2 adds anyfs_kindprobe_meta: spool 2 MB of the partition to a host
 * tmpfile, run libblkid against it for FSTYPE/LABEL/UUID. We snapshot
 * rather than pass libblkid an LKL fd because libblkid uses host
 * pread(2), which has no idea what an LKL kernel fd is.
 */
#define _GNU_SOURCE
#include "kindprobe.h"

#include <lkl.h>
#include <lkl_host.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef ANYFS_HAS_BLKID
#include <blkid/blkid.h>
#endif

#define PROBE_SIZE (64u * 1024u)

static int mem_match(const void* buf, size_t len, size_t off, const void* pat,
		     size_t patlen)
{
	if (off + patlen > len)
		return 0;
	return memcmp((const unsigned char*)buf + off, pat, patlen) == 0;
}

AnyfsPartKind anyfs_kindprobe_buf(const void* buf, size_t len)
{
	if (!buf || len < 512)
		return ANYFS_PART_KIND_FS;

	/* LUKS: "LUKS\xBA\xBE" at offset 0 (both LUKS1 and LUKS2). */
	if (mem_match(buf, len, 0, "LUKS\xBA\xBE", 6))
		return ANYFS_PART_KIND_LUKS;

	/* LVM2 PV: an 8-byte LABELONE sector lives in one of sectors 0..3
	 * of the device. Each candidate label sector starts with
	 *   "LABELONE" at +0
	 * and contains "LVM2 001" near its start (type field at +0x20 of
	 * the label header). Scan all four sectors. */
	for (int s = 0; s < 4; s++) {
		size_t base = (size_t)s * 512u;
		if (base + 64 > len)
			break;
		if (mem_match(buf, len, base, "LABELONE", 8) &&
		    mem_match(buf, len, base + 0x20, "LVM2 001", 8))
			return ANYFS_PART_KIND_LVM_PV;
	}

	/* Nested GPT: "EFI PART" at LBA 1 (offset 512). */
	if (mem_match(buf, len, 512, "EFI PART", 8))
		return ANYFS_PART_KIND_NESTED_PARTITION_TABLE;

	/* Nested MBR: 0x55AA at +510. Has to look like a partition table,
	 * not just any file ending in 55AA — require at least one non-zero
	 * partition-type byte in the four entries at +446..+509.
	 *
	 * Many real filesystems (ext4 e.g.) do NOT carry 0x55AA at +510;
	 * the only false positives we'd worry about are FAT/NTFS, which
	 * use 0x55AA as their own boot signature. Those have a known
	 * OEM/jump pattern at the very start, so reject MBR if either of
	 * those is present. */
	if (len >= 512) {
		const unsigned char* p = (const unsigned char*)buf;
		if (p[510] == 0x55 && p[511] == 0xAA) {
			int looks_mbr = 0;
			for (int i = 0; i < 4; i++) {
				const unsigned char* e = p + 446 + i * 16;
				/* boot-flag must be 0x00 or 0x80; ptype
				 * non-zero; LBA fields non-zero. */
				if ((e[0] == 0x00 || e[0] == 0x80) &&
				    e[4] != 0x00) {
					looks_mbr = 1;
					break;
				}
			}
			/* FAT12/16/FAT32 jump 0xEB ?? 0x90 or 0xE9; NTFS "NTFS
			 * " at +3. Reject if found (it's still KIND_FS). */
			int looks_fat_ntfs = 0;
			if (p[0] == 0xEB || p[0] == 0xE9)
				looks_fat_ntfs = 1;
			if (mem_match(buf, len, 3, "NTFS    ", 8))
				looks_fat_ntfs = 1;
			if (looks_mbr && !looks_fat_ntfs)
				return ANYFS_PART_KIND_NESTED_PARTITION_TABLE;
		}
	}

	return ANYFS_PART_KIND_FS;
}

AnyfsPartKind anyfs_kindprobe_blkdev(const char* lkl_blkdev_path)
{
	if (!lkl_blkdev_path)
		return ANYFS_PART_KIND_FS;

	int fd = lkl_sys_open(lkl_blkdev_path, LKL_O_RDONLY, 0);
	if (fd < 0)
		return ANYFS_PART_KIND_FS;

	unsigned char buf[PROBE_SIZE];
	size_t got = 0;
	while (got < sizeof(buf)) {
		long n = lkl_sys_pread64(fd, (char*)buf + got,
					 sizeof(buf) - got, (long long)got);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	lkl_sys_close(fd);

	if (got == 0)
		return ANYFS_PART_KIND_FS;

	return anyfs_kindprobe_buf(buf, got);
}

/* Spool up to `spool_bytes` from the start of the LKL block device to a
 * fresh anonymous host-side file. Returns a *host* fd (positive) on
 * success that the caller must close, or -1 on any failure. */
static int spool_to_host_tmpfile(const char* lkl_blkdev_path,
				 size_t spool_bytes)
{
	int lfd = lkl_sys_open(lkl_blkdev_path, LKL_O_RDONLY, 0);
	if (lfd < 0)
		return -1;

	/* O_TMPFILE creates an unlinked file we hand directly to libblkid.
	 * Fall back to mkstemp+unlink if O_TMPFILE is unavailable. */
	int hfd = -1;
#ifdef O_TMPFILE
	hfd = open("/tmp", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#endif
	if (hfd < 0) {
		char tmpl[] = "/tmp/anyfs-probe-XXXXXX";
		hfd = mkstemp(tmpl);
		if (hfd >= 0)
			(void)unlink(tmpl);
	}
	if (hfd < 0) {
		lkl_sys_close(lfd);
		return -1;
	}

	char buf[64 * 1024];
	size_t got = 0;
	while (got < spool_bytes) {
		size_t want = spool_bytes - got;
		if (want > sizeof(buf))
			want = sizeof(buf);
		long n = lkl_sys_pread64(lfd, buf, want, (long long)got);
		if (n <= 0)
			break;
		ssize_t w = 0;
		while (w < n) {
			ssize_t m = write(hfd, buf + w, (size_t)(n - w));
			if (m <= 0) {
				lkl_sys_close(lfd);
				close(hfd);
				return -1;
			}
			w += m;
		}
		got += (size_t)n;
	}
	lkl_sys_close(lfd);
	if (got == 0) {
		close(hfd);
		return -1;
	}
	(void)lseek(hfd, 0, SEEK_SET);
	return hfd;
}

/* MBR primary entry is at offset 446 + 16*i, length 16. Layout:
 *   +0  boot flag (0x00 or 0x80)
 *   +1..+3   CHS first
 *   +4  type
 *   +5..+7   CHS last
 *   +8..+11  LBA first (little-endian)
 *   +12..+15 sectors (little-endian)
 */
static uint32_t le32(const unsigned char* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint64_t le64(const unsigned char* p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int parse_mbr(const unsigned char* buf, size_t len, AnyfsInnerPart* out,
		     int max)
{
	if (len < 512)
		return 0;
	if (buf[510] != 0x55 || buf[511] != 0xAA)
		return 0;
	int n = 0;
	for (int i = 0; i < 4 && n < max; i++) {
		const unsigned char* e = buf + 446 + i * 16;
		unsigned int type = e[4];
		if (type == 0x00)
			continue;
		/* Skip protective MBR sentinel (0xEE) — caller handles GPT
		 * path. */
		if (type == 0xEE)
			continue;
		/* Skip extended-partition containers (0x05/0x0F): v2 doesn't
		 * chase the linked list. */
		if (type == 0x05 || type == 0x0F)
			continue;
		uint32_t first_lba = le32(e + 8);
		uint32_t sectors = le32(e + 12);
		if (first_lba == 0 || sectors == 0)
			continue;
		out[n].index = (unsigned int)(i + 1);
		out[n].start_bytes = (uint64_t)first_lba * 512u;
		out[n].size_bytes = (uint64_t)sectors * 512u;
		n++;
	}
	return n;
}

static int parse_gpt(const unsigned char* buf, size_t len, AnyfsInnerPart* out,
		     int max)
{
	/* GPT header sits at LBA 1 (+512). Signature "EFI PART" at +0. */
	if (len < 16 * 1024)
		return 0; /* need header + a few entries */
	const unsigned char* hdr = buf + 512;
	if (memcmp(hdr, "EFI PART", 8) != 0)
		return 0;
	uint32_t n_entries = le32(hdr + 80);
	uint32_t ent_size = le32(hdr + 84);
	uint64_t ent_lba = le64(hdr + 72);
	if (ent_size < 128)
		return 0;
	if (n_entries == 0 || n_entries > 256)
		n_entries = 128;

	uint64_t arr_off = ent_lba * 512u;
	int n = 0;
	for (uint32_t i = 0; i < n_entries && n < max; i++) {
		uint64_t eoff = arr_off + (uint64_t)i * ent_size;
		if (eoff + ent_size > len)
			break;
		const unsigned char* e = buf + eoff;
		/* GPT entry: type GUID at +0 (16 bytes). All-zero = empty. */
		int empty = 1;
		for (int j = 0; j < 16; j++)
			if (e[j]) {
				empty = 0;
				break;
			}
		if (empty)
			continue;
		uint64_t first_lba = le64(e + 32);
		uint64_t last_lba = le64(e + 40);
		if (last_lba < first_lba)
			continue;
		out[n].index = (unsigned int)(i + 1);
		out[n].start_bytes = first_lba * 512u;
		out[n].size_bytes = (last_lba - first_lba + 1) * 512u;
		n++;
	}
	return n;
}

int anyfs_partprobe_buf(const void* raw, size_t len, AnyfsInnerPart* out,
			int max)
{
	if (!raw || !out || max <= 0)
		return 0;
	const unsigned char* buf = (const unsigned char*)raw;
	/* Prefer GPT when both an MBR signature and "EFI PART" coexist: the
	 * MBR is the protective-MBR shell, the real table is GPT. */
	int n = parse_gpt(buf, len, out, max);
	if (n > 0)
		return n;
	return parse_mbr(buf, len, out, max);
}

int anyfs_partprobe_blkdev(const char* lkl_blkdev_path, AnyfsInnerPart* out,
			   int max)
{
	if (!lkl_blkdev_path || !out || max <= 0)
		return 0;
	int fd = lkl_sys_open(lkl_blkdev_path, LKL_O_RDONLY, 0);
	if (fd < 0)
		return 0;

	/* 64 KB is enough for an MBR + a 128-entry GPT (128 * 128 = 16 KB
	 * starting at LBA 2 = +1 KB → ends at +17 KB). */
	unsigned char buf[64 * 1024];
	size_t got = 0;
	while (got < sizeof(buf)) {
		long r = lkl_sys_pread64(fd, (char*)buf + got,
					 sizeof(buf) - got, (long long)got);
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	lkl_sys_close(fd);
	if (got < 512)
		return 0;
	return anyfs_partprobe_buf(buf, got, out, max);
}

int anyfs_kindprobe_meta(const char* lkl_blkdev_path, char fstype[32],
			 char label[64], char uuid[40])
{
	if (fstype)
		fstype[0] = '\0';
	if (label)
		label[0] = '\0';
	if (uuid)
		uuid[0] = '\0';
	if (!lkl_blkdev_path)
		return -1;

#ifdef ANYFS_HAS_BLKID
	/* Spool up to 2 MB — enough for every common superblock (xfs
	 * superblock is at +0, ext at +1024, btrfs primary at +64KB, etc.)
	 * AND for libblkid's secondary checks. */
	int hfd = spool_to_host_tmpfile(lkl_blkdev_path, 2u * 1024u * 1024u);
	if (hfd < 0)
		return -1;

	/* libblkid needs a path. /proc/self/fd/<n> is the standard way to
	 * give it a fd-backed file. */
	char fdpath[64];
	snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", hfd);

	blkid_probe pr = blkid_new_probe_from_filename(fdpath);
	if (!pr) {
		close(hfd);
		return -1;
	}
	blkid_probe_enable_superblocks(pr, 1);
	blkid_probe_set_superblocks_flags(
	    pr, BLKID_SUBLKS_TYPE | BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID);

	int rc = blkid_do_safeprobe(pr);
	if (rc == 0) {
		const char* val = NULL;
		size_t vlen = 0;
		if (fstype &&
		    blkid_probe_lookup_value(pr, "TYPE", &val, &vlen) == 0) {
			size_t cap = 32 - 1;
			if (vlen > cap)
				vlen = cap;
			memcpy(fstype, val, vlen);
			fstype[vlen] = '\0';
		}
		if (label &&
		    blkid_probe_lookup_value(pr, "LABEL", &val, &vlen) == 0) {
			size_t cap = 64 - 1;
			if (vlen > cap)
				vlen = cap;
			memcpy(label, val, vlen);
			label[vlen] = '\0';
		}
		if (uuid &&
		    blkid_probe_lookup_value(pr, "UUID", &val, &vlen) == 0) {
			size_t cap = 40 - 1;
			if (vlen > cap)
				vlen = cap;
			memcpy(uuid, val, vlen);
			uuid[vlen] = '\0';
		}
	}
	blkid_free_probe(pr);
	close(hfd);
	return 0;
#else
	(void)spool_to_host_tmpfile;
	return 0;
#endif
}
