/** Minimal surface of the emscripten module we rely on. */
export interface AnyfsModule {
    HEAPU8: Uint8Array;
    HEAP32: Int32Array;
    HEAPU32: Uint32Array;
    _malloc(n: number): number;
    _free(p: number): void;
    ccall(
        name: string,
        ret: 'number' | 'string' | null,
        argTypes: ReadonlyArray<'number' | 'string' | 'bigint'>,
        args: ReadonlyArray<number | string | bigint>,
        opts?: { async?: boolean },
    ): number | string | Promise<number | string>;
    UTF8ToString(ptr: number, maxBytes?: number): string;
    stringToUTF8(s: string, ptr: number, maxBytes: number): void;
    FS: {
        mkdir(path: string, mode?: number): void;
        mount(type: unknown, opts: unknown, mountpoint: string): void;
        unmount(mountpoint: string): void;
    };
    WORKERFS?: unknown;
    NODEFS?: unknown;
}

export type AnyfsModuleFactory = (opts?: {
    preRun?: Array<(m: AnyfsModule) => void>;
    locateFile?: (path: string, prefix: string) => string;
    print?: (msg: string) => void;
    printErr?: (msg: string) => void;
}) => Promise<AnyfsModule>;
