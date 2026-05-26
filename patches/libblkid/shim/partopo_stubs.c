/* partopo_stubs.c — drop-in replacement for partitions/ and topology/.
 *
 * anyfs only uses libblkid for superblock magic detection on a tmpfile of
 * the first ~2 MiB of an LKL partition. Partition-table probing and disk
 * topology are out of scope. Yet probe.c still references the chain
 * drivers + a couple of API functions. Provide inert stubs so the link
 * succeeds and nothing surprising happens at runtime if a caller ever
 * stumbles into BLKID_CHAIN_PARTS / BLKID_CHAIN_TOPLGY. */

#include <stdlib.h>
#include "blkidP.h"

/* Empty idinfo tables — these chains will simply find no candidates. */
static const struct blkid_idinfo *partitions_idinfos[] = { NULL };
static const struct blkid_idinfo *topology_idinfos[]   = { NULL };

static int stub_probe(blkid_probe pr, struct blkid_chain *chn)
{
	(void)pr; (void)chn;
	return BLKID_PROBE_NONE;
}

const struct blkid_chaindrv partitions_drv = {
	.id           = BLKID_CHAIN_PARTS,
	.name         = "partitions",
	.dflt_enabled = 0,
	.idinfos      = partitions_idinfos,
	.nidinfos     = 0,
	.has_fltr     = 0,
	.probe        = stub_probe,
	.safeprobe    = stub_probe,
	.free_data    = NULL,
};

const struct blkid_chaindrv topology_drv = {
	.id           = BLKID_CHAIN_TOPLGY,
	.name         = "topology",
	.dflt_enabled = 0,
	.idinfos      = topology_idinfos,
	.nidinfos     = 0,
	.has_fltr     = 0,
	.probe        = stub_probe,
	.safeprobe    = stub_probe,
	.free_data    = NULL,
};

/* probe.c calls these from blkid_wipe_all() and
 * blkid_probe_get_wholedisk_probe(). anyfs never invokes those paths. */
int blkid_probe_enable_partitions(blkid_probe pr, int enable)
{
	(void)pr; (void)enable;
	return 0;
}
int blkid_probe_set_partitions_flags(blkid_probe pr, int flags)
{
	(void)pr; (void)flags;
	return 0;
}
int blkid_probe_get_partitions_flags(blkid_probe pr)
{
	(void)pr;
	return 0;
}

/* Public API: a few partition-table queries are part of blkid.h. We don't
 * use them but the linker complains if a downstream user references one. */
blkid_partlist blkid_probe_get_partitions(blkid_probe pr)
{
	(void)pr; return NULL;
}
int blkid_partlist_numof_partitions(blkid_partlist ls)
{
	(void)ls; return 0;
}
blkid_partition blkid_partlist_get_partition(blkid_partlist ls, int n)
{
	(void)ls; (void)n; return NULL;
}
blkid_parttable blkid_partlist_get_table(blkid_partlist ls)
{
	(void)ls; return NULL;
}
const char *blkid_parttable_get_type(blkid_parttable tab)
{
	(void)tab; return NULL;
}

/* Topology API */
int blkid_probe_enable_topology(blkid_probe pr, int enable)
{
	(void)pr; (void)enable;
	return 0;
}
blkid_topology blkid_probe_get_topology(blkid_probe pr)
{
	(void)pr; return NULL;
}
unsigned long blkid_topology_get_alignment_offset(blkid_topology tp)
{
	(void)tp; return 0;
}
unsigned long blkid_topology_get_minimum_io_size(blkid_topology tp)
{
	(void)tp; return 0;
}
unsigned long blkid_topology_get_optimal_io_size(blkid_topology tp)
{
	(void)tp; return 0;
}
unsigned long blkid_topology_get_logical_sector_size(blkid_topology tp)
{
	(void)tp; return 0;
}
unsigned long blkid_topology_get_physical_sector_size(blkid_topology tp)
{
	(void)tp; return 0;
}
unsigned long blkid_topology_get_dax(blkid_topology tp)
{
	(void)tp; return 0;
}
unsigned long blkid_topology_get_diskseq(blkid_topology tp)
{
	(void)tp; return 0;
}
