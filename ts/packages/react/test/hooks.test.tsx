import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, waitFor, cleanup } from '@testing-library/react';
import React from 'react';

const prewarmMock = vi.fn();
vi.mock('@anyfs/core', () => ({
    createSession: () => ({ backend: 'wasm', allowedKinds: new Set(['blob', 'url']), wasmCaps: {} }),
    prewarm: (...args: unknown[]) => prewarmMock(...args),
    prewarmNative: vi.fn(),
    NativeSession: class NativeSessionMock {},
}));

import { AnyfsProvider, useAnyfsDir, useAnyfsFile } from '../src/index.js';

const entriesRoot = [
    { name: 'etc', kind: 'dir' },
    { name: 'README', kind: 'file' },
];

function makeSession() {
    return {
        attachBlob: vi.fn(async () => {}),
        onProgress: vi.fn(() => () => {}),
        close: vi.fn(async () => {}),
        readdir: vi.fn(async (p: string) => {
            if (p === '/') return entriesRoot;
            throw new Error('ENOENT');
        }),
        stat: vi.fn(async () => ({ size: 4, mode: 0o100644 })),
        openFd: vi.fn(async () => 7),
        readFd: vi.fn(async () => new Uint8Array([1, 2, 3, 4])),
        closeFd: vi.fn(async () => {}),
    };
}

function Tree({ path }: { path: string }) {
    const { entries, loading, error } = useAnyfsDir(path);
    if (error) return <div data-testid="dir">error:{error.message}</div>;
    if (loading || !entries) return <div data-testid="dir">loading</div>;
    return <div data-testid="dir">{entries.map((e) => e.name).join(',')}</div>;
}

function FileBytes({ path }: { path: string }) {
    const { data, error } = useAnyfsFile(path);
    if (error) return <div data-testid="file">error</div>;
    return <div data-testid="file">{data ? Array.from(data).join(',') : 'loading'}</div>;
}

const blob = new Blob([new Uint8Array(8)]);

beforeEach(() => prewarmMock.mockReset());
// globals:false means testing-library's auto-cleanup (which hooks the global
// afterEach) never registers — without this, renders leak across tests.
afterEach(cleanup);

describe('useAnyfsDir', () => {
    it('lists entries once ready and caches per (session,path)', async () => {
        const s = makeSession();
        prewarmMock.mockResolvedValue(s);
        const { rerender } = render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js"><Tree path="/" /></AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('dir').textContent).toBe('etc,README'));
        rerender(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js"><Tree path="/" /></AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('dir').textContent).toBe('etc,README'));
        expect(s.readdir).toHaveBeenCalledTimes(1);
    });

    it('surfaces readdir errors', async () => {
        prewarmMock.mockResolvedValue(makeSession());
        render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js"><Tree path="/missing" /></AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('dir').textContent).toBe('error:ENOENT'));
    });
});

describe('useAnyfsFile', () => {
    it('stat + openFd + readFd + closeFd round trip', async () => {
        const s = makeSession();
        prewarmMock.mockResolvedValue(s);
        render(
            <AnyfsProvider source={{ kind: 'blob', blob }} workerUrl="/w.js"><FileBytes path="/README" /></AnyfsProvider>,
        );
        await waitFor(() => expect(screen.getByTestId('file').textContent).toBe('1,2,3,4'));
        expect(s.openFd).toHaveBeenCalledWith('/README');
        expect(s.readFd).toHaveBeenCalledWith(7, 0, 4);
        expect(s.closeFd).toHaveBeenCalledWith(7);
    });
});
