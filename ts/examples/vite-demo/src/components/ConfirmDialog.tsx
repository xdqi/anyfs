function ConfirmDialog({
    title,
    message,
    confirmLabel = 'Confirm',
    onConfirm,
    onCancel,
}: {
    title: string;
    message: string;
    confirmLabel?: string;
    onConfirm: () => void;
    onCancel: () => void;
}) {
    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onCancel();
            else if (e.key === 'Enter') onConfirm();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [onCancel, onConfirm]);
    return (
        <div
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
            onClick={onCancel}
        >
            <div
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-md mx-4 shadow-xl"
                onClick={(e) => e.stopPropagation()}
                role="alertdialog"
                aria-modal="true"
            >
                <header className="px-5 py-4 border-b border-zinc-200 dark:border-zinc-800">
                    <h2 className="text-zinc-900 dark:text-zinc-100 text-lg font-semibold">
                        {title}
                    </h2>
                </header>
                <div className="px-5 py-4 text-base text-zinc-700 dark:text-zinc-300 leading-relaxed">
                    {message}
                </div>
                <div className="px-5 py-4 border-t border-zinc-200 dark:border-zinc-800 flex justify-end gap-2">
                    <button
                        className="rounded-lg border border-zinc-300 dark:border-zinc-700 px-4 py-2 text-sm font-medium text-zinc-700 dark:text-zinc-200 hover:bg-zinc-100 dark:hover:bg-zinc-800"
                        onClick={onCancel}
                    >
                        Cancel
                    </button>
                    <button
                        className="rounded-lg bg-emerald-600 hover:bg-emerald-500 px-4 py-2 text-sm font-medium text-white"
                        onClick={onConfirm}
                        autoFocus
                    >
                        {confirmLabel}
                    </button>
                </div>
            </div>
        </div>
    );
}
