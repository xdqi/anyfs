import { useAnyfsDiskMaybe } from '@anyfs/react';
import { STATUS_BAR_CLS } from './SupportedFormats';

export function KernelStatusBar() {
    const anyfs = useAnyfsDiskMaybe();
    if (!anyfs)
        return <div className={`${STATUS_BAR_CLS} text-zinc-600 dark:text-zinc-400`}>idle</div>;
    let label: string;
    // Light-mode shades are darker (-600/700) so the text reads cleanly on a
    // white background; the -400 dark-mode shades keep the same vibe on zinc-900.
    let cls = 'text-zinc-600 dark:text-zinc-400';
    switch (anyfs.status) {
        case 'booting':
            label = `🔧 booting kernel${anyfs.step ? ` · ${anyfs.step}` : '…'}`;
            cls = 'text-amber-700 dark:text-amber-400';
            break;
        case 'booted':
            label = '✅ kernel ready';
            cls = 'text-emerald-700 dark:text-emerald-400';
            break;
        case 'attaching':
            label = `⏳ attaching disk${anyfs.step ? ` · ${anyfs.step}` : '…'}`;
            cls = 'text-amber-700 dark:text-amber-400';
            break;
        case 'mounting':
            label = `📂 ${anyfs.step ?? 'mounting filesystem…'}`;
            cls = 'text-amber-700 dark:text-amber-400';
            break;
        case 'ready':
            label = '✅ ready';
            cls = 'text-emerald-700 dark:text-emerald-400';
            break;
        case 'error':
            label = `⚠️ ${anyfs.error?.message ?? 'error'}`;
            cls = 'text-red-700 dark:text-red-400';
            break;
        default:
            label = anyfs.status;
    }
    return <div className={`${STATUS_BAR_CLS} ${cls}`}>{label}</div>;
}
