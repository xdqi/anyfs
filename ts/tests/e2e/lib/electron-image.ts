/**
 * Tiny module-level seam so a flow spec can pin the image path BEFORE the
 * Electron app launches. The `driver` fixture launches Electron at use-time
 * (before any page/baseURL exists), but ElectronDriver needs the host image
 * path at LAUNCH (via launchElectron's localImagePath → ANYFS_TEST_LOCAL_PATH).
 * Flow specs targeting the Electron projects call setElectronImage(fx.file) in
 * a test.beforeEach so the fixture can read it back here.
 */
let cur: string | undefined;
export const setElectronImage = (p?: string): void => {
    cur = p;
};
export const getElectronImage = (): string | undefined => cur;
