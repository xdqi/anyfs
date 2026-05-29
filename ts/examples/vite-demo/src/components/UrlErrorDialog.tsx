function UrlErrorDialog({
    title,
    message,
    onClose,
}: {
    title: string;
    message: string;
    onClose: () => void;
}) {
    useEffect(() => {
        const onKey = (e: KeyboardEvent) => {
            if (e.key === 'Escape') onClose();
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [onClose]);
    return (
        <div
            className="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
            onClick={onClose}
        >
            <div
                className="bg-white border border-zinc-300 dark:bg-zinc-900 dark:border-zinc-700 rounded-lg w-full max-w-md mx-4 shadow-xl"
                onClick={(e) => e.stopPropagation()}
                role="alertdialog"
                aria-modal="true"
            >
                <header className="px-4 py-3 border-b border-zinc-200 dark:border-zinc-800 flex items-center justify-between">
                    <h2 className="text-zinc-900 dark:text-zinc-100 text-base font-medium">
                        {title}
                    </h2>
                    <button
                        className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100"
                        onClick={onClose}
                        aria-label="Close"
                    >
                        ×
                    </button>
                </header>
                <div className="p-4 text-sm text-zinc-700 dark:text-zinc-300 leading-relaxed">
                    {message}
                </div>
                <div className="px-4 py-3 border-t border-zinc-200 dark:border-zinc-800 flex justify-end">
                    <button
                        className="rounded-lg bg-emerald-600 hover:bg-emerald-500 px-4 py-2 text-sm font-medium text-white"
                        onClick={onClose}
                        autoFocus
                    >
                        OK
                    </button>
                </div>
            </div>
        </div>
    );
}
