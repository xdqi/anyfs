import { useEffect } from 'react';

export function AboutDialog({ open, onClose }: { open: boolean; onClose: () => void }) {
    useEffect(() => {
        if (!open) return;
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onClose();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [open, onClose]);
    if (!open) return null;

    type Dep = { name: string; license: string; url: string; note?: string };
    const native: Dep[] = [
        {
            name: 'Linux Kernel Library (LKL)',
            license: 'GPL-2.0',
            url: 'https://github.com/lkl/linux',
            note: 'in-process Linux kernel — does all filesystem work',
        },
        {
            name: 'QEMU',
            license: 'GPL-2.0+',
            url: 'https://www.qemu.org',
            note: 'block layer — image format support',
        },
        {
            name: 'util-linux (libblkid)',
            license: 'LGPL-2.1+',
            url: 'https://github.com/util-linux/util-linux',
        },
        { name: 'GLib', license: 'LGPL-2.1+', url: 'https://gitlab.gnome.org/GNOME/glib' },
    ];
    const web: Dep[] = [
        { name: 'React', license: 'MIT', url: 'https://react.dev' },
        { name: 'Vite', license: 'MIT', url: 'https://vitejs.dev' },
        { name: 'Tailwind CSS', license: 'MIT', url: 'https://tailwindcss.com' },
        { name: 'Chonky', license: 'MIT', url: 'https://github.com/TimboKZ/Chonky' },
        {
            name: 'chonky-icon-fontawesome',
            license: 'MIT',
            url: 'https://github.com/TimboKZ/Chonky',
        },
    ];

    const linkCls = 'text-emerald-700 dark:text-emerald-400 hover:underline';
    const sectionH =
        'text-xs font-semibold uppercase tracking-wide text-zinc-500 dark:text-zinc-400 mt-4 mb-2';

    const renderList = (deps: Dep[]) => (
        <ul className="space-y-1.5">
            {deps.map((d) => (
                <li
                    key={d.name}
                    className="text-sm text-zinc-700 dark:text-zinc-300 flex items-baseline gap-2 flex-wrap"
                >
                    <a href={d.url} target="_blank" rel="noopener noreferrer" className={linkCls}>
                        {d.name}
                    </a>
                    <span className="text-xs text-zinc-500 dark:text-zinc-400 font-mono">
                        {d.license}
                    </span>
                    {d.note && (
                        <span className="text-xs text-zinc-500 dark:text-zinc-400">— {d.note}</span>
                    )}
                </li>
            ))}
        </ul>
    );

    return (
        <div
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
            onClick={onClose}
        >
            <div
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-lg mx-4 shadow-xl flex flex-col max-h-[85vh]"
                onClick={(e) => e.stopPropagation()}
                role="dialog"
                aria-modal="true"
                aria-labelledby="about-dialog-title"
            >
                <header className="px-5 py-4 border-b border-zinc-200 dark:border-zinc-800">
                    <h2
                        id="about-dialog-title"
                        className="text-zinc-900 dark:text-zinc-100 text-lg font-semibold"
                    >
                        About anyfs reader
                    </h2>
                </header>
                <div className="px-5 py-4 overflow-y-auto">
                    <p className="text-sm text-zinc-700 dark:text-zinc-300 leading-relaxed">
                        Read any Linux-supported filesystem from your browser — no root, no FUSE, no
                        kernel modules. Powered by a Linux kernel running in-tab via WebAssembly.
                    </p>
                    <div className={sectionH}>License</div>
                    <p className="text-sm text-zinc-700 dark:text-zinc-300 leading-relaxed">
                        Licensed under the{' '}
                        <a
                            href="https://www.gnu.org/licenses/old-licenses/gpl-2.0.html"
                            target="_blank"
                            rel="noopener noreferrer"
                            className={linkCls}
                        >
                            GNU General Public License v2.0
                        </a>{' '}
                        (GPL-2.0). Source code is available on GitHub.
                    </p>
                    <div className={sectionH}>Native components</div>
                    {renderList(native)}
                    <div className={sectionH}>Web stack</div>
                    {renderList(web)}
                    <p className="mt-4 text-xs text-zinc-500 dark:text-zinc-400 leading-relaxed">
                        Each component is the property of its respective authors and is used under
                        its own license. Visit the linked project pages for the full license text
                        and copyright notices.
                    </p>
                </div>
                <div className="px-5 py-4 border-t border-zinc-200 dark:border-zinc-800 flex justify-end">
                    <button
                        className="rounded-lg bg-emerald-600 hover:bg-emerald-500 px-4 py-2 text-sm font-medium text-white"
                        onClick={onClose}
                        autoFocus
                    >
                        Close
                    </button>
                </div>
            </div>
        </div>
    );
}

// Subset of drivelist-anyfs's Drive/Partition that we actually consume.
export type SysMountpoint = { path: string; label: string | null };
export type SysPartition = {
    device: string;
    size: number | null;
    number: number | null;
    fstype: string | null;
    label: string | null;
    uuid: string | null;
    partlabel: string | null;
    parttype: string | null;
    mountpoints: SysMountpoint[];
    isReadOnly: boolean;
};
export type SysDrive = {
    device: string;
    description: string;
    size: number | null;
    busType: string;
    isRemovable: boolean;
    isSystem: boolean;
    isVirtual: boolean | null;
    partitionTableType: 'mbr' | 'gpt' | null;
    partitions: SysPartition[] | null;
};

export type ElectronDrives = { list: () => Promise<SysDrive[] | null> };
