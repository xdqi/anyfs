/** Per partition: at most 500 entries and 6 levels, then 64 KiB from each of
 *  at most 20 files. */
export const LIMITS = { entries: 500, depth: 6, files: 20, readBytes: 64 * 1024 };

/**
 * Walk `root` breadth-first within `limits`, then read the files found.
 * An op that rejects is recorded and the walk goes on: one bad inode must
 * not hide the rest of the tree. Stops once `stopped()` is true (the
 * session went fatal). Returns { entries, files, bytes, errors }.
 */
export async function walkAndRead(
    session,
    root,
    { stopped = () => false, onStep = () => {}, limits = LIMITS } = {},
) {
    const errors = [];
    const attempt = async (op, path, fn) => {
        try {
            return await fn();
        } catch (e) {
            errors.push({ op, path, message: e instanceof Error ? e.message : String(e) });
            return undefined;
        }
    };

    onStep('walk');
    const files = [];
    let entries = 0;
    const queue = [{ path: root, depth: 0 }];
    while (queue.length > 0 && entries < limits.entries && !stopped()) {
        const { path, depth } = queue.shift();
        const list = await attempt('readdir', path, () => session.readdir(path));
        if (!list) continue;
        for (const e of list) {
            if (e.name === '.' || e.name === '..') continue;
            if (entries >= limits.entries || stopped()) break;
            entries++;
            const child = path.endsWith('/') ? `${path}${e.name}` : `${path}/${e.name}`;
            const st = await attempt('stat', child, () => session.stat(child));
            const kind = st?.kind ?? e.kind;
            if (kind === 'link') await attempt('readlink', child, () => session.readlink(child));
            else if (kind === 'dir' && depth + 1 < limits.depth)
                queue.push({ path: child, depth: depth + 1 });
            else if (kind === 'file' && files.length < limits.files) files.push(child);
        }
    }

    onStep('read');
    let bytes = 0;
    for (const f of files) {
        if (stopped()) break;
        const fd = await attempt('open', f, () => session.openFd(f));
        if (fd === undefined) continue;
        const data = await attempt('read', f, () => session.readFd(fd, 0, limits.readBytes));
        if (data) bytes += data.length;
        await attempt('close', f, () => session.closeFd(fd));
    }
    return { entries, files: files.length, bytes, errors };
}
