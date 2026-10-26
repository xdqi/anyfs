import { test, expect } from '@playwright/test';

test.describe('theme', () => {
    test('applies dark mode and keeps the picker legible', async ({ page }) => {
        await page.emulateMedia({ colorScheme: 'dark' });
        await page.goto('/');
        await expect
            .poll(() => page.evaluate(() => document.documentElement.classList.contains('dark')))
            .toBe(true);

        await page.screenshot({
            path: process.env.ANYFS_UI_SCREENSHOT_DIR
                ? process.env.ANYFS_UI_SCREENSHOT_DIR + '/web-dark-picker.png'
                : undefined,
            fullPage: true,
        });

        await page.getByRole('button', { name: 'Settings' }).click();
        await page.getByRole('radio', { name: 'Dark' }).click();
        await expect
            .poll(() => page.evaluate(() => document.documentElement.classList.contains('dark')))
            .toBe(true);
        await page.screenshot({
            path: process.env.ANYFS_UI_SCREENSHOT_DIR
                ? process.env.ANYFS_UI_SCREENSHOT_DIR + '/web-dark-settings.png'
                : undefined,
            fullPage: true,
        });

        await page.getByRole('radio', { name: 'Light' }).click();
        await expect
            .poll(() => page.evaluate(() => document.documentElement.classList.contains('dark')))
            .toBe(false);
    });
});
