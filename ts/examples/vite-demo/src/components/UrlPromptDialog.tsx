import { useEffect, useState } from 'react';
import { getUrlProxyPrefix } from '@anyfs/core';
import { probeUrlAhead } from '../utils';

// Modal prompt for an http(s)/ftp URL. Triggered from the "Open URL…" link
// on the landing page. Resolves with the entered URL or null on cancel.
// Probes the URL in browser mode (HEAD via probeUrlAhead so CORS/404 errors
// surface here rather than as a worker crash); in native mode QEMU's curl
// driver handles probing itself, so we accept any non-empty string.
export function UrlPromptDialog({
    onSubmit,
    onClose,
    mode,
}: {
    onSubmit: (url: string) => void;
    onClose: () => void;
    mode: 'native' | 'wasm';
}) {
    const [url, setUrl] = useState('');
    const [probing, setProbing] = useState(false);
    const [err, setErr] = useState<string | null>(null);

    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onClose();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [onClose]);

    const submit = async () => {
        const trimmed = url.trim();
        if (!trimmed) return;
        if (mode === 'wasm') {
            setProbing(true);
            setErr(null);
            try {
                await probeUrlAhead(trimmed);
            } catch (e) {
                setErr((e as Error).message);
                setProbing(false);
                return;
            }
            setProbing(false);
        }
        onSubmit(trimmed);
    };

    return (
        <div
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
            onClick={onClose}
        >
            <div
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-lg mx-4 shadow-xl"
                onClick={(e) => e.stopPropagation()}
                role="dialog"
                aria-modal="true"
            >
                <header className="px-5 py-4 border-b border-zinc-200 dark:border-zinc-800">
                    <h2 className="text-zinc-900 dark:text-zinc-100 text-lg font-semibold">
                        Open URL
                    </h2>
                </header>
                <div className="px-5 py-4 space-y-3">
                    <input
                        type="url"
                        value={url}
                        onChange={(e) => setUrl(e.target.value)}
                        onKeyDown={(e) => {
                            if (e.key === 'Enter') void submit();
                        }}
                        placeholder="https://example.com/image.qcow2"
                        disabled={probing}
                        autoFocus
                        className="w-full rounded-lg bg-zinc-100 border border-zinc-300 text-zinc-900 placeholder:text-zinc-400 dark:bg-zinc-800 dark:border-zinc-700 dark:text-zinc-100 dark:placeholder:text-zinc-500 px-3 py-2 text-base focus:outline-none focus:border-emerald-500 disabled:opacity-60"
                        aria-label="Disk image URL"
                    />
                    {err && <div className="text-sm text-rose-500">{err}</div>}
                    <p className="text-sm text-zinc-500 leading-relaxed">
                        {mode === 'native'
                            ? 'Fetched by the native QEMU curl block driver — http(s), ftp, sftp; range requests preferred.'
                            : `Fetched in 512 KiB chunks via HTTP Range requests — the server must support range responses${
                                  getUrlProxyPrefix() ? '.' : ' and CORS.'
                              }`}
                    </p>
                </div>
                <div className="px-5 py-4 border-t border-zinc-200 dark:border-zinc-800 flex justify-end gap-2">
                    <button
                        className="rounded-lg border border-zinc-300 dark:border-zinc-700 px-4 py-2 text-sm font-medium text-zinc-700 dark:text-zinc-200 hover:bg-zinc-100 dark:hover:bg-zinc-800"
                        onClick={onClose}
                    >
                        Cancel
                    </button>
                    <button
                        className="rounded-lg bg-emerald-600 hover:bg-emerald-500 disabled:bg-zinc-300 disabled:text-zinc-500 dark:disabled:bg-zinc-700 dark:disabled:text-zinc-500 px-4 py-2 text-sm font-medium text-white"
                        onClick={() => void submit()}
                        disabled={url.trim().length === 0 || probing}
                    >
                        {probing ? 'Probing…' : 'Open'}
                    </button>
                </div>
            </div>
        </div>
    );
}
