import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, waitFor, cleanup, act } from '@testing-library/react';
import React from 'react';

const fakeSession = () => ({
    attachBlob: vi.fn(async () => {}),
    attachUrl: vi.fn(async () => {}),
    attachPath: vi.fn(async () => {}),
    onProgress: vi.fn(() => () => {}),
    onFatal: vi.fn(() => () => {}),
    close: vi.fn(async () => {}),
    readdir: vi.fn(async () => []),
    stat: vi.fn(async () => ({ size: 0, mode: 0o100644 })),
    openFd: vi.fn(async () => 3),
    readFd: vi.fn(async () => new Uint8Array(0)),
    closeFd: vi.fn(async () => {}),
});

const prewarmMock = vi.fn();

vi.mock('@anyfs/core', () => ({
    createSession: () => ({
        backend: 'wasm',
        allowedKinds: new Set(['blob', 'url']),
        wasmCaps: {},
    }),
    prewarm: (...args: unknown[]) => prewarmMock(...args),
    prewarmNative: vi.fn(),
    NativeSession: class NativeSessionMock {},
}));

import { AnyfsProvider, useAnyfsDisk } from '../src/index.js';

function Status() {
    const { status, error } = useAnyfsDisk();
    return (
        <div data-testid="status">
            {status}
            {error ? `:${error.message}` : ''}
        </div>
    );
}

function SessionProbe() {
    const { session } = useAnyfsDisk();
    return <div data-testid="session">{session ? 'session' : 'none'}</div>;
}

beforeEach(() => {
    prewarmMock.mockReset();
});
// globals:false means testing-library's auto-cleanup (which hooks the global
// afterEach) never registers — without this, renders leak across tests.
afterEach(cleanup);

describe('AnyfsProvider', () => {
    it('idle without source or prewarm', () => {
        render(
            <AnyfsProvider source={null} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        expect(screen.getByTestId('status').textContent).toBe('idle');
    });

    it('prewarm → booting → booted', async () => {
        let resolve!: (s: unknown) => void;
        prewarmMock.mockReturnValue(new Promise((r) => (resolve = r)));
        render(
            <AnyfsProvider source={null} workerUrl="/w.js" prewarm>
                <Status />
            </AnyfsProvider>,
        );
        expect(screen.getByTestId('status').textContent).toBe('booting');
        resolve(fakeSession());
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('booted'));
    });

    it('blob source attaches and reaches ready', async () => {
        const s = fakeSession();
        prewarmMock.mockResolvedValue(s);
        const blob = new Blob([new Uint8Array(16)]);
        render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('ready'));
        expect(s.attachBlob).toHaveBeenCalledWith(blob);
    });

    it('disallowed source kind → error state', async () => {
        prewarmMock.mockResolvedValue(fakeSession());
        render(
            <AnyfsProvider source={{ kind: 'path', path: '/dev/sda' } as never} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() =>
            expect(screen.getByTestId('status').textContent).toMatch(/^error:.*not supported/),
        );
    });

    it('prewarm failure → error state', async () => {
        prewarmMock.mockRejectedValue(new Error('boot failed'));
        render(
            <AnyfsProvider source={null} workerUrl="/w.js" prewarm>
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() =>
            expect(screen.getByTestId('status').textContent).toBe('error:boot failed'),
        );
    });

    it('attach failure reaches error even if close() never settles', async () => {
        const s = fakeSession();
        s.attachBlob.mockRejectedValue(new Error('attach boom'));
        s.close = vi.fn(() => new Promise<void>(() => {}));
        prewarmMock.mockResolvedValue(s);
        const blob = new Blob([new Uint8Array(16)]);
        render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() =>
            expect(screen.getByTestId('status').textContent).toBe('error:attach boom'),
        );
        expect(s.close).toHaveBeenCalled();
    });

    it('forwards mountOpts.opTimeoutMs to prewarm', async () => {
        prewarmMock.mockResolvedValue(fakeSession());
        render(
            <AnyfsProvider
                source={null}
                workerUrl="/w.js"
                prewarm
                mountOpts={{ opTimeoutMs: 5000 }}
            >
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() =>
            expect(prewarmMock).toHaveBeenCalledWith(
                expect.objectContaining({ opTimeoutMs: 5000 }),
            ),
        );
    });

    it('onFatal after attach → error state, session dropped, close called', async () => {
        const s = fakeSession();
        let fatal!: (e: Error) => void;
        s.onFatal = vi.fn((cb: (e: Error) => void) => {
            fatal = cb;
            return () => {};
        });
        prewarmMock.mockResolvedValue(s);
        const blob = new Blob([new Uint8Array(16)]);
        render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js">
                <Status />
                <SessionProbe />
            </AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('ready'));
        expect(screen.getByTestId('session').textContent).toBe('session');
        const closesBefore = s.close.mock.calls.length;
        act(() => fatal(new Error('engine wedged')));
        await waitFor(() =>
            expect(screen.getByTestId('status').textContent).toBe('error:engine wedged'),
        );
        expect(screen.getByTestId('session').textContent).toBe('none');
        expect(s.close.mock.calls.length).toBeGreaterThan(closesBefore);
    });

    it('onFatal from a superseded session is ignored', async () => {
        const a = fakeSession();
        const b = fakeSession();
        let fatalA!: (e: Error) => void;
        a.onFatal = vi.fn((cb: (e: Error) => void) => {
            fatalA = cb;
            return () => {};
        });
        prewarmMock.mockResolvedValueOnce(a).mockResolvedValueOnce(b);
        const blob1 = new Blob([new Uint8Array(16)]);
        const blob2 = new Blob([new Uint8Array(32)]);
        const { rerender } = render(
            <AnyfsProvider source={{ kind: 'blob', blob: blob1 }} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() => expect(a.attachBlob).toHaveBeenCalled());
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('ready'));
        rerender(
            <AnyfsProvider source={{ kind: 'blob', blob: blob2 }} workerUrl="/w.js">
                <Status />
            </AnyfsProvider>,
        );
        await waitFor(() => expect(b.attachBlob).toHaveBeenCalledWith(blob2));
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('ready'));
        act(() => fatalA(new Error('stale boom')));
        expect(screen.getByTestId('status').textContent).toBe('ready');
    });
});
