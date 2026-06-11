import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, waitFor, cleanup } from '@testing-library/react';
import React from 'react';

const fakeSession = () => ({
    attachBlob: vi.fn(async () => {}),
    attachUrl: vi.fn(async () => {}),
    attachPath: vi.fn(async () => {}),
    onProgress: vi.fn(() => () => {}),
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
    return <div data-testid="status">{status}{error ? `:${error.message}` : ''}</div>;
}

beforeEach(() => { prewarmMock.mockReset(); });
// globals:false means testing-library's auto-cleanup (which hooks the global
// afterEach) never registers — without this, renders leak across tests.
afterEach(cleanup);

describe('AnyfsProvider', () => {
    it('idle without source or prewarm', () => {
        render(<AnyfsProvider source={null} workerUrl="/w.js"><Status /></AnyfsProvider>);
        expect(screen.getByTestId('status').textContent).toBe('idle');
    });

    it('prewarm → booting → booted', async () => {
        let resolve!: (s: unknown) => void;
        prewarmMock.mockReturnValue(new Promise((r) => (resolve = r)));
        render(<AnyfsProvider source={null} workerUrl="/w.js" prewarm><Status /></AnyfsProvider>);
        expect(screen.getByTestId('status').textContent).toBe('booting');
        resolve(fakeSession());
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('booted'));
    });

    it('blob source attaches and reaches ready', async () => {
        const s = fakeSession();
        prewarmMock.mockResolvedValue(s);
        const blob = new Blob([new Uint8Array(16)]);
        render(<AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js"><Status /></AnyfsProvider>);
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('ready'));
        expect(s.attachBlob).toHaveBeenCalledWith(blob);
    });

    it('disallowed source kind → error state', async () => {
        prewarmMock.mockResolvedValue(fakeSession());
        render(<AnyfsProvider source={{ kind: 'path', path: '/dev/sda' } as never} workerUrl="/w.js"><Status /></AnyfsProvider>);
        await waitFor(() => expect(screen.getByTestId('status').textContent).toMatch(/^error:.*not supported/));
    });

    it('prewarm failure → error state', async () => {
        prewarmMock.mockRejectedValue(new Error('boot failed'));
        render(<AnyfsProvider source={null} workerUrl="/w.js" prewarm><Status /></AnyfsProvider>);
        await waitFor(() => expect(screen.getByTestId('status').textContent).toBe('error:boot failed'));
    });
});
