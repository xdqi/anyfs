function TopBar({
    onOpenSettings,
    onOpenAbout,
    onHomeClick,
    imageName,
    onImageClick,
    partitionLabel,
}: {
    onOpenSettings: () => void;
    onOpenAbout: () => void;
    onHomeClick?: () => void;
    imageName?: string;
    onImageClick?: () => void;
    partitionLabel?: string;
}) {
    const titleClasses = 'text-zinc-900 dark:text-zinc-100 text-base font-semibold tracking-tight';
    const crumbBtn =
        'text-zinc-700 dark:text-zinc-300 text-base hover:text-emerald-600 dark:hover:text-emerald-400 transition-colors max-w-[20rem] truncate';
    const crumbCur = 'text-zinc-500 dark:text-zinc-400 text-base max-w-[20rem] truncate';
    const sep = (
        <span aria-hidden="true" className="text-zinc-400 dark:text-zinc-600 select-none">
            ›
        </span>
    );
    return (
        <header className="border-b border-zinc-200 dark:border-zinc-800 bg-white dark:bg-zinc-950 px-4 h-14 flex items-center justify-between gap-3">
            <nav aria-label="Breadcrumb" className="flex items-center gap-2 min-w-0">
                {onHomeClick ? (
                    <button
                        className={`${titleClasses} hover:text-emerald-600 dark:hover:text-emerald-400 transition-colors`}
                        onClick={onHomeClick}
                        title="Close this disk and return to the picker"
                    >
                        anyfs reader
                    </button>
                ) : (
                    <span className={titleClasses}>anyfs reader</span>
                )}
                {imageName && (
                    <>
                        {sep}
                        {onImageClick ? (
                            <button
                                className={crumbBtn}
                                onClick={onImageClick}
                                title="Return to the partition list"
                            >
                                {imageName}
                            </button>
                        ) : (
                            <span className={crumbCur} title={imageName}>
                                {imageName}
                            </span>
                        )}
                    </>
                )}
                {partitionLabel && (
                    <>
                        {sep}
                        <span className={crumbCur}>{partitionLabel}</span>
                    </>
                )}
            </nav>
            <div className="flex items-center gap-1 shrink-0">
                <a
                    href="https://github.com/xdqi/anyfs"
                    target="_blank"
                    rel="noopener noreferrer"
                    className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100 p-1.5 inline-flex items-center justify-center"
                    aria-label="View source on GitHub"
                    title="View source on GitHub"
                >
                    <svg
                        viewBox="0 0 16 16"
                        width="20"
                        height="20"
                        fill="currentColor"
                        aria-hidden="true"
                    >
                        <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0 0 16 8c0-4.42-3.58-8-8-8z" />
                    </svg>
                </a>
                <button
                    className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100 p-1.5 inline-flex items-center justify-center"
                    onClick={onOpenAbout}
                    aria-label="About"
                    title="About"
                >
                    <svg
                        viewBox="0 0 16 16"
                        width="20"
                        height="20"
                        fill="none"
                        stroke="currentColor"
                        strokeWidth="1.5"
                        aria-hidden="true"
                    >
                        <circle cx="8" cy="8" r="6.5" />
                        <line x1="8" y1="7" x2="8" y2="11.5" strokeLinecap="round" />
                        <circle cx="8" cy="4.7" r="0.85" fill="currentColor" stroke="none" />
                    </svg>
                </button>
                <button
                    className="text-zinc-500 hover:text-zinc-900 dark:text-zinc-400 dark:hover:text-zinc-100 text-xl leading-none p-1"
                    onClick={onOpenSettings}
                    aria-label="Settings"
                    title="Settings"
                >
                    ⚙
                </button>
            </div>
        </header>
    );
}
