import { useRef, useState } from 'react';
import type { DragEvent as ReactDragEvent } from 'react';

export function DropOverlay({ onDrop }: { onDrop: (files: FileList) => void }) {
    // Full-viewport fixed overlay. We use a ref counter so dragleave from
    // a child doesn't flicker the overlay off, and a state bool for the CSS.
    const depth = useRef(0);
    const [dragging, setDragging] = useState(false);

    const onDragEnter = (e: ReactDragEvent) => {
        e.preventDefault();
        depth.current++;
        if (depth.current === 1) setDragging(true);
    };
    const onDragOver = (e: ReactDragEvent) => {
        e.preventDefault();
        (e.dataTransfer as DataTransfer).dropEffect = 'copy';
    };
    const onDragLeave = (e: ReactDragEvent) => {
        depth.current--;
        if (depth.current <= 0) {
            depth.current = 0;
            setDragging(false);
        }
    };
    const onDropHere = (e: ReactDragEvent) => {
        e.preventDefault();
        depth.current = 0;
        setDragging(false);
        if (e.dataTransfer.files.length > 0) onDrop(e.dataTransfer.files);
    };

    return (
        <div
            className={`fixed inset-0 z-50 flex items-center justify-center transition-opacity duration-150 ${
                dragging
                    ? 'bg-emerald-500/10 opacity-100 pointer-events-auto'
                    : 'pointer-events-none opacity-0'
            }`}
            onDragEnter={onDragEnter}
            onDragOver={onDragOver}
            onDragLeave={onDragLeave}
            onDrop={onDropHere}
        >
            <div className="bg-white dark:bg-zinc-900 border-2 border-dashed border-emerald-400 rounded-2xl px-8 py-6 shadow-2xl text-center">
                <p className="text-emerald-700 dark:text-emerald-400 text-lg font-semibold">
                    Drop image here
                </p>
                <p className="text-zinc-500 text-sm mt-1">
                    .img .iso .qcow2 .vmdk .vdi .vhd .vhdx .dmg
                </p>
            </div>
        </div>
    );
}
