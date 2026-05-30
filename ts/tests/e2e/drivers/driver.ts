import type { Fixture, TreeEntry } from '../fixtures/manifest';

export type DownloadMechanism = 'service-worker' | 'electron-ipc';

export interface DownloadResult {
    bytes: Uint8Array;
    size: number;
    mechanism: DownloadMechanism;
}

export type ErrorKind = 'bad-image' | 'no-range' | 'unsupported' | 'mount-failed';

export interface RowInfo {
    name: string;
    isDir: boolean;
}

export interface PropsInfo {
    /** raw text content of the size cell as the app renders it (may be
     *  human-formatted like "1.2 KiB", NOT necessarily a raw byte count) */
    sizeText: string | null;
    kind: string | null;
}

export interface Driver {
    /** Boot the app to a ready picker. */
    start(): Promise<void>;
    stop(): Promise<void>;

    /** Source injection. */
    openImage(fx: Fixture): Promise<void>;
    openUrl(url: string): Promise<void>;

    /** Disk/partition. */
    listPartitionIndices(): Promise<number[]>;
    enterPartition(index: number): Promise<void>;
    /** Step back from a mounted partition to the partition list. */
    backToPartitions(): Promise<void>;

    /** Filesystem browsing (current dir). */
    listRows(): Promise<RowInfo[]>;
    navigateInto(name: string): Promise<void>;
    propertiesOf(name: string): Promise<PropsInfo>;

    /** Download the named file from the current dir. Reports which mechanism
     *  actually fired (SW on web, IPC on electron) — never papered over. */
    download(name: string): Promise<DownloadResult>;

    /** Error assertions — wait for the app to surface the given failure. */
    expectError(kind: ErrorKind): Promise<void>;

    /** The resolved backend after ready ('native' | 'wasm' | 'node-wasm'). */
    backendMode(): Promise<string | null>;
}

export type { Fixture, TreeEntry };
