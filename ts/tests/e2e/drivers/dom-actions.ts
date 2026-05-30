import type { Page } from '@playwright/test';
import type { ErrorKind, PropsInfo, RowInfo } from './driver';

// Chonky 2.3.2 renders each file-list row as a <div data-test-id="file-entry">
// carrying a data-chonky-file-id attribute equal to the row id (mount-relative
// path, e.g. "README.TXT" or "dir/nested.bin"). The skeleton/loader rows that
// Chonky shows mid-readdir have NO data-chonky-file-id, so requiring that
// attribute filters them out. The display name is the last path segment of the
// id (the outer file-entry div has no title attribute — confirmed live; the
// name lives on the inner FileEntryName span instead, so we read it off the id).
// Directory detection isn't a DOM attribute: dirs render a fontawesome folder
// icon (data-icon="folder"), files render data-icon="file-alt" etc. — verified
// live (file-alt) and from chonky-icon-fontawesome source (isDir → faFolder).
export const ROW = '[data-test-id="file-entry"][data-chonky-file-id]';

/**
 * Shared, Page-driven DOM actions used by BOTH WebDriver and ElectronDriver.
 * The renderer DOM is identical across the two environments (the Electron app
 * ships the same vite-demo build), so everything below the source-injection /
 * download mechanics is common. Each function takes a Playwright `Page` and
 * uses the exact selectors confirmed live against the Chonky file list.
 */

/** Read the published __anyfsTest.getState() snapshot, or null if absent. */
export function getState(page: Page): Promise<{
    status: string;
    mode: string | null;
    mountPath: string | null;
    error: { message: string } | null;
} | null> {
    return page.evaluate(() => (window as any).__anyfsTest?.getState() ?? null);
}

/** The resolved backend after ready ('native' | 'wasm' | 'node-wasm' | null). */
export function backendMode(page: Page): Promise<string | null> {
    return page.evaluate(() => (window as any).__anyfsTest?.getState().mode ?? null);
}

/** Block until the test bridge is installed (getState is callable). */
export async function waitForBridge(page: Page, timeout = 60_000): Promise<void> {
    await page.waitForFunction(() => !!(window as any).__anyfsTest?.getState, null, { timeout });
}

/** Block until getState().status leaves 'idle' (a source actually took). */
export async function waitForSourceLoaded(page: Page, timeout = 30_000): Promise<void> {
    await page.waitForFunction(
        () => (window as any).__anyfsTest?.getState().status !== 'idle',
        null,
        { timeout },
    );
}

/**
 * Block until the disk reaches 'ready' (partition table read) or 'error'.
 * Needed where prewarm leaves status at 'booted' before a source is attached —
 * there 'status !== idle' is already true, so it can't tell whether a source
 * actually took. 'ready'/'error' both indicate the attach finished one way or
 * the other.
 */
export async function waitForReadyOrError(page: Page, timeout = 120_000): Promise<void> {
    await page.waitForFunction(
        () => {
            const s = (window as any).__anyfsTest?.getState().status;
            return s === 'ready' || s === 'error';
        },
        null,
        { timeout },
    );
}

export async function listPartitionIndices(page: Page): Promise<number[]> {
    // The partition picker renders one button per partition with
    // data-testid="partition-<index>" (index 0 = whole disk). Wait for at
    // least one to appear, then read the numeric suffixes.
    await page.locator('[data-testid^="partition-"]').first().waitFor({
        state: 'visible',
        timeout: 120_000,
    });
    const ids = await page.locator('[data-testid^="partition-"]').evaluateAll((nodes) =>
        nodes
            .map((n) => (n as HTMLElement).dataset.testid ?? '')
            .map((t) => Number.parseInt(t.replace('partition-', ''), 10))
            .filter((n) => Number.isFinite(n)),
    );
    return ids.sort((a, b) => a - b);
}

export async function enterPartition(page: Page, index: number): Promise<void> {
    await page.locator(`[data-testid="partition-${index}"]`).click();
    // The disk session is already 'ready' (status reflects the whole-disk
    // attach, not the per-partition mount), so we wait on the actual mount
    // result: either the Chonky file list appears (a file-entry row, even
    // a loader skeleton) or DiskView surfaces a mount error inline. We do
    // NOT swallow a hung mount — if neither happens, the wait times out and
    // the caller sees the failure.
    await page
        .locator('[data-test-id="file-entry"], [class*="fileListEmpty"]')
        .first()
        .waitFor({ state: 'attached', timeout: 120_000 });
}

export async function backToPartitions(page: Page): Promise<void> {
    // The TopBar breadcrumb (nav[aria-label="Breadcrumb"]) renders the disk
    // image name as a button when inside a partition; clicking it fires
    // App.askBackToParts which pops a ConfirmDialog ("Return to the
    // partition list?"). We must click the crumb AND confirm. The image
    // crumb is the button titled "Return to the partition list".
    await page
        .locator('nav[aria-label="Breadcrumb"] button[title="Return to the partition list"]')
        .click();
    // ConfirmDialog has no testid; it's a role="alertdialog" whose confirm
    // button text is the confirmLabel ("Back").
    const dialog = page.locator('[role="alertdialog"]');
    await dialog.waitFor({ state: 'visible', timeout: 30_000 });
    await dialog.getByRole('button', { name: 'Back', exact: true }).click();
    // Back at the picker the partition buttons reappear.
    await page.locator('[data-testid^="partition-"]').first().waitFor({
        state: 'visible',
        timeout: 30_000,
    });
}

export async function listRows(page: Page): Promise<RowInfo[]> {
    // Wait for the file list to settle: either real rows (file-entry with a
    // chonky-file-id) or an explicitly-empty list. We give the readdir a
    // moment to replace the loader skeletons.
    await page.waitForFunction(
        () => {
            const list = document.querySelector('.chonky-fileList, [class*="fileList"]');
            if (!list) return false;
            // Loader skeleton rows have no data-chonky-file-id; once readdir
            // resolves, real rows carry it (or the list is empty).
            const skeletons = list.querySelectorAll(
                '[data-test-id="file-entry"]:not([data-chonky-file-id])',
            );
            return skeletons.length === 0;
        },
        null,
        { timeout: 60_000 },
    );
    return page.locator(ROW).evaluateAll((nodes) =>
        nodes.map((node) => {
            const el = node as HTMLElement;
            const id = el.dataset.chonkyFileId ?? '';
            const name = id.split('/').pop() ?? id;
            // Directory detection: Chonky's folder icon renders a
            // fontawesome <svg> with data-icon="folder" (closed) for dirs.
            // Files get a file/file-* icon. Fall back to false.
            const svg = el.querySelector('svg[data-icon]');
            const icon = svg?.getAttribute('data-icon') ?? '';
            const isDir = icon === 'folder' || icon === 'folder-open';
            return { name, isDir };
        }),
    );
}

export async function navigateInto(page: Page, name: string): Promise<void> {
    // Chonky opens a directory on double-click. The row is identified by its
    // data-chonky-file-id; at the current dir that id ends with the name.
    const row = rowByName(page, name);
    await row.waitFor({ state: 'visible', timeout: 30_000 });
    await row.dblclick();
}

export async function propertiesOf(page: Page, name: string): Promise<PropsInfo> {
    // Select the row first (the Properties action is requiresSelection),
    // then right-click to open Chonky's MUI context menu, then click the
    // Properties item. Chonky mounts a MUI <Menu> per dropdown (toolbar +
    // context menu), so several role="menu" containers exist; the OPEN one
    // is the only with a visible menuitem. Target the menuitem by role+name
    // and click the visible one rather than scoping role="menu" (which is
    // ambiguous — strict mode resolves it to multiple closed menus).
    const row = rowByName(page, name);
    await row.waitFor({ state: 'visible', timeout: 30_000 });
    await row.click();
    await row.click({ button: 'right' });
    // Chonky's context menu renders each entry as an MUI <li role="menuitem">
    // whose text is the action name; the Properties item carries the
    // info-circle icon. Several MUI menus coexist in the DOM (toolbar
    // dropdowns), but only the context menu has a "Properties" item, so this
    // locator is unambiguous. Click drives Playwright's actionability wait.
    const properties = page.locator('li[role="menuitem"]', { hasText: 'Properties' });
    await properties.first().click({ timeout: 30_000 });

    const modal = page.locator('[data-testid="properties-modal"]');
    await modal.waitFor({ state: 'visible', timeout: 30_000 });
    const sizeText = await textOrNull(page, '[data-testid="properties-size"]');
    const kind = await textOrNull(page, '[data-testid="properties-kind"]');
    return { sizeText, kind };
}

export async function expectError(page: Page, _kind: ErrorKind): Promise<void> {
    // The app surfaces failures in two places:
    //   - URL/open failures pop the UrlErrorDialog (data-testid url-error-dialog)
    //   - mount/boot failures render inline in DiskView ("Error: …" or
    //     "Can't mount partition #N"), and getState().status becomes 'error'.
    // Resolve when ANY of these is observed. We don't assert the specific
    // kind here — that's the test's job; the driver just waits for the
    // error surface to appear.
    await Promise.race([
        page
            .locator('[data-testid="url-error-dialog"]')
            .waitFor({ state: 'visible', timeout: 120_000 }),
        page.waitForFunction(
            () => (window as any).__anyfsTest?.getState().status === 'error',
            null,
            { timeout: 120_000 },
        ),
        page
            .getByText(/Can.t mount partition #/i)
            .first()
            .waitFor({ state: 'visible', timeout: 120_000 }),
    ]);
}

// ── helpers ──────────────────────────────────────────────────────────────

/** Locate a file-list row by its display name (last segment of the id). */
export function rowByName(page: Page, name: string) {
    // Match the row whose data-chonky-file-id equals the name (root level)
    // or ends with "/name" (nested). CSS attribute selectors give us both:
    // an exact match OR a suffix match guarded by the slash.
    return page.locator(
        `${ROW}[data-chonky-file-id="${cssEscape(name)}"], ` +
            `${ROW}[data-chonky-file-id$="/${cssEscape(name)}"]`,
    );
}

async function textOrNull(page: Page, selector: string): Promise<string | null> {
    const loc = page.locator(selector);
    if ((await loc.count()) === 0) return null;
    const t = await loc.first().textContent();
    return t === null ? null : t.trim();
}

/** Minimal CSS attribute-value escaper for the names we feed selectors. */
export function cssEscape(value: string): string {
    return value.replace(/(["\\])/g, '\\$1');
}
