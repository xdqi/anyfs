import type { WasmApiModule } from './wasm-api.js';

/** Minimal surface of the emscripten module we rely on. */
export interface AnyfsModule extends WasmApiModule {
    HEAPU32: Uint32Array;
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
