import { expect } from '@playwright/test';
import type { Driver } from '../drivers/driver';
import type { PartExpect } from '../fixtures/manifest';

/** Assert the current directory's rows contain every top-level entry the
 *  manifest names for this partition (subset check — not exhaustive). */
export async function expectKnownTree(driver: Driver, part: PartExpect): Promise<void> {
    const rows = await driver.listRows();
    const names = new Set(rows.map((r) => r.name));
    const tops = new Set(part.tree.map((e) => e.path.split('/')[0]));
    for (const top of tops) {
        expect(names, `row "${top}" present in current dir`).toContain(top);
    }
}
