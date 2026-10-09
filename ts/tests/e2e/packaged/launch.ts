import { _electron, type ElectronApplication } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { basename } from 'node:path';

// Launch a packaged app for Playwright. On windows-2025, electron.launch has
// timed out with the app already running and CDP connected (electron run
// 37937273260's GUI smoke, first attempt; 37955347237's device spec). A
// launch timeout fails the test even when caught, so the retry is
// packaged.config.ts's `retries` (reported as flaky); this only bounds the
// launch, prints what was running and kills a stray app first.
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
        console.warn(`[launchPackaged] ${first}`);
        killStray(exe);
        throw e;
    }
}

function killStray(exe: string): void {
    try {
        const list =
            process.platform === 'win32'
                ? execFileSync('tasklist', ['/v', '/fi', `IMAGENAME eq ${basename(exe)}`], {
                      encoding: 'utf8',
                  })
                : execFileSync('ps', ['axo', 'pid,ppid,etime,stat,command'], { encoding: 'utf8' })
                      .split('\n')
                      .filter((l) => l.includes(basename(exe)))
                      .join('\n');
        console.warn(`[launchPackaged] processes before cleanup:\n${list}`);
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
