import { createRequire } from 'node:module';
import { NATIVE_ADDON } from './paths.mjs';

/**
 * The addon as an AnyfsNativeBridge (the shape the Electron preload hands
 * NativeSession), so the native backend runs through the same session code
 * — op watchdog included — as the app. Mirrors the IPC handlers in
 * examples/electron-demo/src/main.ts.
 */
export function nativeBridge() {
    const addon = createRequire(import.meta.url)(NATIVE_ADDON);
    const unsupported = async () => {
        throw new Error('not supported in the robustness harness');
    };
    return {
        available: async () => true,
        init: (memMb, loglevel) => addon.kernelInit(memMb >>> 0, loglevel >>> 0),
        diskOpen: (path, flags) => addon.sessionOpen(path, flags >>> 0),
        diskClose: (h) => addon.sessionClose(h),
        diskListJson: (h) => addon.sessionListJson(h),
        diskMetaJson: (h) => addon.sessionMetaJson(h),
        diskEnter: (h, part, flags) => addon.sessionEnter(h, part >>> 0, flags >>> 0),
        readdirJson: (p) => addon.readdirJson(p),
        lstatJson: (p) => addon.lstatJson(p),
        statJson: (p) => addon.statJson(p),
        realpath: (p) => addon.realpath(p),
        readlink: (p) => addon.readlink(p),
        startProxy: unsupported,
        stopProxy: unsupported,
        fileOpen: (p, flags) => addon.fileOpen(p, flags >>> 0),
        pread: (fd, n, off) => addon.pread(fd, n, off),
        fileClose: (fd) => addon.fileClose(fd),
        onFatal: (cb) => {
            addon.onFatal(cb);
            return () => {};
        },
    };
}
