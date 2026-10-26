import { _electron, type ElectronApplication, type Page } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { basename } from 'node:path';

export async function launchPackaged(
    exe: string,
    env: Record<string, string>,
    timeout = 120_000,
): Promise<ElectronApplication> {
    try {
        return await _electron.launch({
            executablePath: exe,
            args: process.platform === 'linux' ? ['--no-sandbox'] : [],
            env,
            timeout,
        });
    } catch (e) {
        const first = (e instanceof Error ? e.message : String(e)).split('\n')[0];
        console.warn('[launchPackaged] ' + first);
        killStray(exe);
        throw e;
    }
}

/** Apply ANYFS_UI_THEME through the same persisted setting used by Settings. */
export async function applyPackagedTheme(page: Page): Promise<void> {
    const theme = process.env.ANYFS_UI_THEME;
    if (theme !== 'dark' && theme !== 'light' && theme !== 'system') return;
    await page.evaluate((value) => {
        let current: Record<string, unknown> = {};
        try {
            current = JSON.parse(localStorage.getItem('anyfs.settings.v1') ?? '{}');
        } catch {
            // Start with defaults if the profile has no valid settings.
        }
        localStorage.setItem('anyfs.settings.v1', JSON.stringify({ ...current, theme: value }));
    }, theme);
    await page.reload({ waitUntil: 'domcontentloaded', timeout: 120_000 });
    if (theme === 'dark') {
        await page.waitForFunction(
            () => document.documentElement.classList.contains('dark'),
            undefined,
            { timeout: 30_000 },
        );
    } else {
        await page.waitForSelector('#root > *', { timeout: 30_000 });
    }
}

function killStray(exe: string): void {
    try {
        const list =
            process.platform === 'win32'
                ? execFileSync('tasklist', ['/v', '/fi', 'IMAGENAME eq ' + basename(exe)], {
                      encoding: 'utf8',
                  })
                : execFileSync('ps', ['axo', 'pid,ppid,etime,stat,command'], { encoding: 'utf8' })
                      .split('\n')
                      .filter((l) => l.includes(basename(exe)))
                      .join('\n');
        console.warn('[launchPackaged] processes before cleanup:\n' + list);
    } catch {
        // listing is best effort
    }
    try {
        if (process.platform === 'win32') {
            execFileSync('taskkill', ['/F', '/T', '/IM', basename(exe)], { stdio: 'ignore' });
        } else {
            execFileSync('pkill', ['-x', basename(exe)], { stdio: 'ignore' });
        }
    } catch {
        // nothing left to kill
    }
}
