import type { Download, Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import type { Fixture } from '../fixtures/manifest';
import type { Driver, DownloadResult, ErrorKind, PropsInfo, RowInfo } from './driver';

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
const ROW = '[data-test-id="file-entry"][data-chonky-file-id]';

export class WebDriver implements Driver {
    constructor(private page: Page) {}

    async start(): Promise<void> {
        // ?e2e=1 activates the __anyfsTest bridge on the production preview.
        await this.page.goto('/?e2e=1');
        await this.page.waitForFunction(() => !!(window as any).__anyfsTest?.getState);
    }

    async stop(): Promise<void> {
        // Nothing to tear down; the Playwright fixture closes the page/context.
    }

    async openImage(fx: Fixture): Promise<void> {
        // Feed a real File from disk through the hidden <input type=file> the
        // FilePicker always keeps mounted (testid legacy-file-input). This
        // avoids serialising multi-MB image bytes through page.evaluate — the
        // generated multi.img is 64 MiB. setInputFiles works on hidden inputs.
        // The input's onChange routes into acceptBlob → fileToSource → onSource,
        // exactly the production load path.
        const input = this.page.locator('[data-testid="legacy-file-input"]');
        await input.waitFor({ state: 'attached', timeout: 30_000 });
        await input.setInputFiles(fx.file);

        // Confirm the source actually took: status must leave 'idle'. If the
        // legacy input is ignored (e.g. an FSA-only build path), fall back to
        // the __anyfsTest.setSourceFile evaluate path with a real File built
        // from the bytes we read here.
        try {
            await this.page.waitForFunction(
                () => (window as any).__anyfsTest?.getState().status !== 'idle',
                null,
                { timeout: 15_000 },
            );
        } catch {
            const buf = readFileSync(fx.file);
            await this.page.evaluate(
                async ({ bytes, name }) => {
                    const file = new File([new Uint8Array(bytes)], name);
                    (window as any).__anyfsTest.setSourceFile(file);
                },
                { bytes: Array.from(buf), name: fx.name },
            );
            await this.page.waitForFunction(
                () => (window as any).__anyfsTest?.getState().status !== 'idle',
                null,
                { timeout: 120_000 },
            );
        }
    }

    async openUrl(url: string): Promise<void> {
        await this.page.evaluate((u) => (window as any).__anyfsTest.openUrl(u), url);
    }

    async listPartitionIndices(): Promise<number[]> {
        // The partition picker renders one button per partition with
        // data-testid="partition-<index>" (index 0 = whole disk). Wait for at
        // least one to appear, then read the numeric suffixes.
        await this.page.locator('[data-testid^="partition-"]').first().waitFor({
            state: 'visible',
            timeout: 120_000,
        });
        const ids = await this.page.locator('[data-testid^="partition-"]').evaluateAll((nodes) =>
            nodes
                .map((n) => (n as HTMLElement).dataset.testid ?? '')
                .map((t) => Number.parseInt(t.replace('partition-', ''), 10))
                .filter((n) => Number.isFinite(n)),
        );
        return ids.sort((a, b) => a - b);
    }

    async enterPartition(index: number): Promise<void> {
        await this.page.locator(`[data-testid="partition-${index}"]`).click();
        // The disk session is already 'ready' (status reflects the whole-disk
        // attach, not the per-partition mount), so we wait on the actual mount
        // result: either the Chonky file list appears (a file-entry row, even
        // a loader skeleton) or DiskView surfaces a mount error inline. We do
        // NOT swallow a hung mount — if neither happens, the wait times out and
        // the caller sees the failure.
        await this.page
            .locator('[data-test-id="file-entry"], [class*="fileListEmpty"]')
            .first()
            .waitFor({ state: 'attached', timeout: 120_000 });
    }

    async backToPartitions(): Promise<void> {
        // The TopBar breadcrumb (nav[aria-label="Breadcrumb"]) renders the disk
        // image name as a button when inside a partition; clicking it fires
        // App.askBackToParts which pops a ConfirmDialog ("Return to the
        // partition list?"). We must click the crumb AND confirm. The image
        // crumb is the button titled "Return to the partition list".
        await this.page
            .locator('nav[aria-label="Breadcrumb"] button[title="Return to the partition list"]')
            .click();
        // ConfirmDialog has no testid; it's a role="alertdialog" whose confirm
        // button text is the confirmLabel ("Back").
        const dialog = this.page.locator('[role="alertdialog"]');
        await dialog.waitFor({ state: 'visible', timeout: 30_000 });
        await dialog.getByRole('button', { name: 'Back', exact: true }).click();
        // Back at the picker the partition buttons reappear.
        await this.page.locator('[data-testid^="partition-"]').first().waitFor({
            state: 'visible',
            timeout: 30_000,
        });
    }

    async listRows(): Promise<RowInfo[]> {
        // Wait for the file list to settle: either real rows (file-entry with a
        // chonky-file-id) or an explicitly-empty list. We give the readdir a
        // moment to replace the loader skeletons.
        await this.page.waitForFunction(
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
        return this.page.locator(ROW).evaluateAll((nodes) =>
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

    async navigateInto(name: string): Promise<void> {
        // Chonky opens a directory on double-click. The row is identified by its
        // data-chonky-file-id; at the current dir that id ends with the name.
        const row = this.rowByName(name);
        await row.waitFor({ state: 'visible', timeout: 30_000 });
        await row.dblclick();
    }

    async propertiesOf(name: string): Promise<PropsInfo> {
        // Select the row first (the Properties action is requiresSelection),
        // then right-click to open Chonky's MUI context menu, then click the
        // Properties item. Chonky mounts a MUI <Menu> per dropdown (toolbar +
        // context menu), so several role="menu" containers exist; the OPEN one
        // is the only with a visible menuitem. Target the menuitem by role+name
        // and click the visible one rather than scoping role="menu" (which is
        // ambiguous — strict mode resolves it to multiple closed menus).
        const row = this.rowByName(name);
        await row.waitFor({ state: 'visible', timeout: 30_000 });
        await row.click();
        await row.click({ button: 'right' });
        // Chonky's context menu renders each entry as an MUI <li role="menuitem">
        // whose text is the action name; the Properties item carries the
        // info-circle icon. Several MUI menus coexist in the DOM (toolbar
        // dropdowns), but only the context menu has a "Properties" item, so this
        // locator is unambiguous. Click drives Playwright's actionability wait.
        const properties = this.page.locator('li[role="menuitem"]', { hasText: 'Properties' });
        await properties.first().click({ timeout: 30_000 });

        const modal = this.page.locator('[data-testid="properties-modal"]');
        await modal.waitFor({ state: 'visible', timeout: 30_000 });
        const sizeText = await this.textOrNull('[data-testid="properties-size"]');
        const kind = await this.textOrNull('[data-testid="properties-kind"]');
        return { sizeText, kind };
    }

    async download(name: string): Promise<DownloadResult> {
        // Activating a file (double-click) fires onFileActivate → streamDownload
        // → the service-worker path on web (Content-Disposition: attachment via
        // a hidden iframe → SW intercept). Capture the browser download event,
        // then read the bytes off the saved file.
        const row = this.rowByName(name);
        await row.waitFor({ state: 'visible', timeout: 30_000 });
        const [download] = await Promise.all([
            this.page.waitForEvent('download', { timeout: 120_000 }),
            row.dblclick(),
        ]);
        const path = await (download as Download).path();
        if (!path) throw new Error('download produced no file path');
        const bytes = new Uint8Array(readFileSync(path));
        return { bytes, size: bytes.byteLength, mechanism: 'service-worker' };
    }

    async expectError(_kind: ErrorKind): Promise<void> {
        // The app surfaces failures in two places:
        //   - URL/open failures pop the UrlErrorDialog (data-testid url-error-dialog)
        //   - mount/boot failures render inline in DiskView ("Error: …" or
        //     "Can't mount partition #N"), and getState().status becomes 'error'.
        // Resolve when ANY of these is observed. We don't assert the specific
        // kind here — that's the test's job; the driver just waits for the
        // error surface to appear.
        await Promise.race([
            this.page
                .locator('[data-testid="url-error-dialog"]')
                .waitFor({ state: 'visible', timeout: 120_000 }),
            this.page.waitForFunction(
                () => (window as any).__anyfsTest?.getState().status === 'error',
                null,
                { timeout: 120_000 },
            ),
            this.page
                .getByText(/Can.t mount partition #/i)
                .first()
                .waitFor({ state: 'visible', timeout: 120_000 }),
        ]);
    }

    async backendMode(): Promise<string | null> {
        return this.page.evaluate(() => (window as any).__anyfsTest?.getState().mode ?? null);
    }

    // ── helpers ──────────────────────────────────────────────────────────

    /** Locate a file-list row by its display name (last segment of the id). */
    private rowByName(name: string) {
        // Match the row whose data-chonky-file-id equals the name (root level)
        // or ends with "/name" (nested). CSS attribute selectors give us both:
        // an exact match OR a suffix match guarded by the slash.
        return this.page.locator(
            `${ROW}[data-chonky-file-id="${cssEscape(name)}"], ` +
                `${ROW}[data-chonky-file-id$="/${cssEscape(name)}"]`,
        );
    }

    private async textOrNull(selector: string): Promise<string | null> {
        const loc = this.page.locator(selector);
        if ((await loc.count()) === 0) return null;
        const t = await loc.first().textContent();
        return t === null ? null : t.trim();
    }
}

/** Minimal CSS attribute-value escaper for the names we feed selectors. */
function cssEscape(value: string): string {
    return value.replace(/(["\\])/g, '\\$1');
}
