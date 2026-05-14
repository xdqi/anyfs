/*
 * anyfs_disk_dump.h — Shared table printer.
 *
 * Same output for `anyfs-lspart` and the `.partitions` synthetic file.
 */
#ifndef ANYFS_DISK_DUMP_H
#define ANYFS_DISK_DUMP_H

#include "anyfs_disk.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Print the header line. */
void anyfs_dump_header(FILE* f);

/* Print one disk's rows. `disk_idx` is the positional 0-based index
 * the surface assigned (drives the `diskN/pM` PATH column). */
void anyfs_dump_disk(FILE* f, AnyfsDisk* d, int disk_idx);

#ifdef __cplusplus
}
#endif

#endif
