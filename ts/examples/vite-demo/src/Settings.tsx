import { createContext, useCallback, useContext, useEffect, useState } from 'react';
import type { ReactNode } from 'react';

export type Theme = 'dark' | 'light' | 'system';

export interface Settings {
    followSymlinks: boolean;
    cacheChunks: boolean;
    theme: Theme;
    /** When true, force the WASM worker path even when the Electron native
     *  addon is available. Useful for comparing backends or working around
     *  native-specific issues. Has no effect in a pure browser environment. */
    disableNative: boolean;
}

const DEFAULT_SETTINGS: Settings = {
    followSymlinks: true,
    cacheChunks: true,
    theme: 'system',
    disableNative: false,
};

const STORAGE_KEY = 'anyfs.settings.v1';

interface Ctx {
    settings: Settings;
    update: <K extends keyof Settings>(key: K, value: Settings[K]) => void;
    /** Theme actually in effect right now (after resolving 'system'). */
    resolvedTheme: 'dark' | 'light';
}

const SettingsCtx = createContext<Ctx | null>(null);

function load(): Settings {
    if (typeof localStorage === 'undefined') return DEFAULT_SETTINGS;
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) return DEFAULT_SETTINGS;
        const parsed = JSON.parse(raw) as Partial<Settings>;
        return { ...DEFAULT_SETTINGS, ...parsed };
    } catch {
        return DEFAULT_SETTINGS;
    }
}

function resolveTheme(t: Theme): 'dark' | 'light' {
    if (t === 'system') {
        return typeof matchMedia !== 'undefined' &&
            matchMedia('(prefers-color-scheme: dark)').matches
            ? 'dark'
            : 'light';
    }
    return t;
}

export function SettingsProvider({ children }: { children: ReactNode }) {
    const [settings, setSettings] = useState<Settings>(load);
    const [resolvedTheme, setResolvedTheme] = useState<'dark' | 'light'>(() =>
        resolveTheme(settings.theme),
    );

    useEffect(() => {
        try {
            localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));
        } catch {
            /* quota / disabled — settings stay session-only */
        }
    }, [settings]);

    // Theme application: toggle the `dark` class on <html> and listen to OS
    // preference changes when set to 'system'.
    useEffect(() => {
        const apply = () => {
            const r = resolveTheme(settings.theme);
            setResolvedTheme(r);
            document.documentElement.classList.toggle('dark', r === 'dark');
        };
        apply();
        if (settings.theme !== 'system') return;
        const mq = matchMedia('(prefers-color-scheme: dark)');
        mq.addEventListener('change', apply);
        return () => mq.removeEventListener('change', apply);
    }, [settings.theme]);

    const update = useCallback(<K extends keyof Settings>(key: K, value: Settings[K]) => {
        setSettings((s) => ({ ...s, [key]: value }));
    }, []);

    return (
        <SettingsCtx.Provider value={{ settings, update, resolvedTheme }}>
            {children}
        </SettingsCtx.Provider>
    );
}

export function useSettings(): Ctx {
    const ctx = useContext(SettingsCtx);
    if (!ctx) throw new Error('useSettings outside SettingsProvider');
    return ctx;
}

interface SettingsDialogProps {
    open: boolean;
    onClose: () => void;
    /** Whether the native addon bridge is available. When true, a toggle
     *  appears so the user can force the WASM worker path instead. */
    nativeAvailable?: boolean;
}

export function SettingsDialog({ open, onClose, nativeAvailable }: SettingsDialogProps) {
    const { settings, update } = useSettings();

    useEffect(() => {
        if (!open) return;
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onClose();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [open, onClose]);

    if (!open) return null;

    return (
        <div
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
            onClick={onClose}
        >
            <div
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-md mx-4 shadow-xl"
                onClick={(e) => e.stopPropagation()}
            >
                <header className="px-4 py-3 border-b border-zinc-200 dark:border-zinc-800 flex items-center justify-between">
                    <h2 className="text-zinc-900 dark:text-zinc-100 text-base font-medium">
                        Settings
                    </h2>
                    <button
                        className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100"
                        onClick={onClose}
                        aria-label="Close"
                    >
                        ×
                    </button>
                </header>
                <div className="p-4 space-y-4">
                    <ThemeChoice value={settings.theme} onChange={(v) => update('theme', v)} />
                    <Toggle
                        label="Follow symlinks"
                        description="Resolve symlink-to-directory to its canonical target when navigating (e.g. /bin → /usr/bin in the breadcrumb)."
                        checked={settings.followSymlinks}
                        onChange={(v) => update('followSymlinks', v)}
                    />
                    <Toggle
                        label="Cache image chunks locally"
                        description="When loading an image from a URL, store fetched 512 KiB ranges in IndexedDB so subsequent reads skip the network. No effect on locally-dropped images."
                        checked={settings.cacheChunks}
                        onChange={(v) => update('cacheChunks', v)}
                    />
                    {nativeAvailable && (
                        <Toggle
                            label="Disable native module"
                            description="Force the WASM worker path even though the native Electron addon is loaded. Changing this requires a page reload to take effect."
                            checked={settings.disableNative}
                            onChange={(v) => update('disableNative', v)}
                        />
                    )}
                </div>
            </div>
        </div>
    );
}

interface ThemeChoiceProps {
    value: Theme;
    onChange: (v: Theme) => void;
}

function ThemeChoice({ value, onChange }: ThemeChoiceProps) {
    const opts: { v: Theme; label: string; icon: string }[] = [
        { v: 'light', label: 'Light', icon: '☀' },
        { v: 'dark', label: 'Dark', icon: '☾' },
        { v: 'system', label: 'System', icon: '🖥' },
    ];
    return (
        <div>
            <div className="text-zinc-900 dark:text-zinc-100 text-sm mb-2">Theme</div>
            <div
                role="radiogroup"
                aria-label="Theme"
                className="inline-flex rounded-lg border border-zinc-300 dark:border-zinc-700 p-0.5 bg-zinc-100 dark:bg-zinc-800"
            >
                {opts.map((o) => {
                    const active = o.v === value;
                    return (
                        <button
                            key={o.v}
                            role="radio"
                            aria-checked={active}
                            data-theme-option={o.v}
                            onClick={() => onChange(o.v)}
                            className={[
                                'px-3 py-1 rounded-md text-xs font-medium transition-colors flex items-center gap-1.5',
                                active
                                    ? 'bg-white text-zinc-900 dark:bg-zinc-700 dark:text-zinc-100 shadow-sm'
                                    : 'text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100',
                            ].join(' ')}
                        >
                            <span aria-hidden="true">{o.icon}</span>
                            {o.label}
                        </button>
                    );
                })}
            </div>
            <div className="text-zinc-500 dark:text-zinc-400 text-xs mt-1.5">
                Affects this page and the embedded file browser.
            </div>
        </div>
    );
}

interface ToggleProps {
    label: string;
    description: string;
    checked: boolean;
    onChange: (v: boolean) => void;
}

function Toggle({ label, description, checked, onChange }: ToggleProps) {
    return (
        <label className="flex items-start gap-3 cursor-pointer select-none">
            <input
                type="checkbox"
                checked={checked}
                onChange={(e) => onChange(e.target.checked)}
                className="mt-1 h-4 w-4 accent-emerald-500"
            />
            <span>
                <span className="block text-zinc-900 dark:text-zinc-100 text-sm">{label}</span>
                <span className="block text-zinc-600 dark:text-zinc-400 text-xs mt-0.5">
                    {description}
                </span>
            </span>
        </label>
    );
}
