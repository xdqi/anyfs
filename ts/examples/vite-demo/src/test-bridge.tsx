import { useEffect } from 'react';
import { useAnyfsDiskMaybe } from '@anyfs/react';

/** True when debug hooks should be active: Vite dev, or explicit ?e2e=1 opt-in
 *  (the latter lets Playwright drive the production preview build). */
export function e2eEnabled(): boolean {
    if (import.meta.env.DEV) return true;
    try {
        return new URLSearchParams(window.location.search).has('e2e');
    } catch {
        return false;
    }
}

/** Publishes a read-only state snapshot onto window.__anyfsTest.getState /
 *  .lastError. Source-injection setters live in App.tsx. Renders nothing. */
export function TestStateBridge() {
    const state = useAnyfsDiskMaybe();
    useEffect(() => {
        if (!e2eEnabled()) return;
        const w = window as any;
        const api = (w.__anyfsTest ??= {});
        api.getState = () => ({
            status: state?.status ?? 'idle',
            mode: state?.mode ?? null,
            mountPath: state?.mountPath ?? null,
            error: state?.error ? { message: state.error.message } : null,
        });
        api.lastError = state?.error ? { message: state.error.message } : null;
    }, [state]);
    return null;
}
