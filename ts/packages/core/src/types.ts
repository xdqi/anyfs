export type DiskHandle = number;
export type LklFd = number;

export interface PartInfo {
    slot_id: number;
    parent: number;
    index: number;
    offset: number;
    size: number;
    ptype: string;
    kind: string;
    fstype: string;
    label: string;
    uuid: string;
}

export type EntryKind = 'dir' | 'file' | 'link' | 'other';

export interface DirEntry {
    name: string;
    ino: number;
    kind: EntryKind;
}

export interface Stat {
    ino: number;
    mode: number;
    size: number;
    nlink: number;
    mtime: number;
    atime: number;
    ctime: number;
    kind: EntryKind;
    /** Owning user id (raw kernel value). Optional: older wasm builds omit. */
    uid?: number;
    /** Owning group id. */
    gid?: number;
    /** Device the inode lives on, encoded as Linux dev_t. */
    dev?: number;
    /** For block/char device nodes, the device they represent. */
    rdev?: number;
    /** Preferred I/O block size (bytes). */
    blksize?: number;
    /** 512-byte block count (st_blocks convention). */
    blocks?: number;
}

export interface MountOpts {
    /** LKL ram size (MiB). Default 64. */
    memMb?: number;
    /** LKL loglevel (0=silent, 7=debug). Default 0. */
    loglevel?: number;
    /** Force whole-disk mount with this fstype, skipping partition probe. */
    forceFstype?: string;
}

export interface EnterOpts {
    fstype?: string;
    flags?: number;
}

export interface DiskMeta {
    /** Total logical (virtual block device) size in bytes. */
    logical_size: number;
    /** Outer partition-table flavour: "gpt", "dos", or "" if no PT detected. */
    pt_type: string;
}
