import { resolve } from 'node:path';
import { IMAGES_DIR } from '../lib/paths';

export interface TreeEntry {
    /** mount-relative path, no leading slash */
    path: string;
    size?: number; // bytes, for regular files
    dir?: boolean; // true for directories
    symlink?: string; // link target, for symlinks
}

export interface PartExpect {
    /** index as listParts reports it (0 = whole disk) */
    index: number;
    fs?: string; // 'ext4' | 'vfat' | 'btrfs' | 'iso9660' | undefined
    tree: TreeEntry[]; // a representative subset to assert (not exhaustive)
}

export interface Fixture {
    name: string;
    source: 'generated' | 'downloaded';
    /** absolute local path once built/fetched */
    file: string;
    /** for downloaded fixtures */
    url?: string;
    expectedSize?: number; // bytes, guard for downloaded
    sha256?: string; // guard for downloaded (filled in once known)
    /** partitions to assert; for whole-disk-no-PT use a single index 0 */
    parts: PartExpect[];
}

const img = (f: string) => resolve(IMAGES_DIR, f);

export const FIXTURES: Record<string, Fixture> = {
    multiRaw: {
        name: 'multiRaw',
        source: 'generated',
        file: img('multi.img'),
        parts: [
            {
                index: 1,
                fs: 'ext4',
                tree: [
                    { path: 'hello.txt', size: 13 },
                    { path: 'dir/nested.bin', size: 4096 },
                    { path: 'empty', dir: true },
                    { path: 'link', symlink: 'hello.txt' },
                ],
            },
            {
                index: 2,
                fs: 'vfat',
                tree: [{ path: 'README.TXT', size: 12 }],
            },
        ],
    },
    mbrExtended: {
        name: 'mbrExtended',
        source: 'generated',
        file: img('mbr-extended.img'),
        // 2 primary (1,2) + logical partitions (5,6) inside an extended (3).
        parts: [
            { index: 1, fs: 'ext4', tree: [{ path: 'p1.txt', size: 3 }] },
            { index: 2, fs: 'vfat', tree: [{ path: 'P2.TXT', size: 3 }] },
            { index: 5, fs: 'ext4', tree: [{ path: 'l5.txt', size: 3 }] },
            { index: 6, fs: 'ext4', tree: [{ path: 'l6.txt', size: 3 }] },
        ],
    },
    btrfsVmdk: {
        name: 'btrfsVmdk',
        source: 'generated',
        file: img('btrfs-whole.vmdk'),
        // whole-disk btrfs, no partition table -> single synthetic index 0.
        parts: [
            {
                index: 0,
                fs: 'btrfs',
                tree: [
                    { path: 'whole.txt', size: 11 },
                    { path: 'sub', dir: true },
                ],
            },
        ],
    },
    qcow2Url: {
        name: 'qcow2Url',
        source: 'downloaded',
        file: img('trusty-cloud.qcow2'),
        url: 'https://cloud-images.ubuntu.com/trusty/current/trusty-server-cloudimg-amd64-disk1.img',
        expectedSize: 264897024,
        sha256: '3c4ad0defbe729dd3c16d2851d775575d1c5351c85734418d3b89bfdfd28ebd1',
        parts: [{ index: 1, fs: 'ext4', tree: [{ path: 'etc/hostname' }] }],
    },
    isoUrl: {
        name: 'isoUrl',
        source: 'downloaded',
        file: img('trusty.iso'),
        url: 'https://releases.ubuntu.com/trusty/ubuntu-14.04.6-server-amd64.iso',
        expectedSize: 662700032,
        sha256: 'b17d7c1e9d0321ad5810ba77b69aef43f0f29a5422b08120e6ee0576c4527c0e',
        parts: [{ index: 0, fs: 'iso9660', tree: [{ path: 'README.diskdefines' }] }],
    },
};
