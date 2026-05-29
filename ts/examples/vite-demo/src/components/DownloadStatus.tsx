import { fmtBytes } from '@anyfs/core';

function DownloadStatus({ job, onDismiss }: { job: DownloadJob; onDismiss: () => void }) {
    const pct = job.size > 0 ? Math.min(100, (job.written / job.size) * 100) : 0;
    return (
        <div className="rounded-md bg-zinc-100 dark:bg-zinc-800 px-4 py-3 text-base flex items-center gap-3 text-zinc-900 dark:text-zinc-100">
            <div className="flex-1 min-w-0">
                <div className="truncate">{job.name}</div>
                {job.error ? (
                    <div className="text-red-500 dark:text-red-400 text-sm mt-1">{job.error}</div>
                ) : (
                    <div className="text-zinc-600 dark:text-zinc-400 text-sm mt-1">
                        {fmtBytes(job.written)} / {fmtBytes(job.size)} ({pct.toFixed(1)}%)
                    </div>
                )}
                {!job.error && (
                    <div className="h-1.5 bg-zinc-300 dark:bg-zinc-700 mt-1.5 rounded overflow-hidden">
                        <div className="h-full bg-emerald-500" style={{ width: `${pct}%` }} />
                    </div>
                )}
            </div>
            {job.error ? (
                <button
                    className="text-zinc-700 hover:text-zinc-900 dark:text-zinc-300 dark:hover:text-white text-sm"
                    onClick={onDismiss}
                >
                    ×
                </button>
            ) : (
                <button
                    className="text-zinc-700 hover:text-zinc-900 dark:text-zinc-300 dark:hover:text-white text-sm"
                    onClick={() => {
                        job.cancel();
                        onDismiss();
                    }}
                >
                    cancel
                </button>
            )}
        </div>
    );
}
