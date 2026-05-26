import { createRoot } from 'react-dom/client';
import { App } from './App';
import { ensureDownloadServiceWorker } from './stream-download';
import './styles.css';

// Register the streaming-download SW early so it's active by the time the
// user activates a file. Fire-and-forget; errors don't block the UI.
// Skip in Electron — main-process IPC handles downloads via electronDownload,
// and the anyfs:// custom scheme can't serve a SW with scope '/' anyway
// (the scope-allowed header isn't honored for non-http origins).
const hasElectronDownload = !!(window as { electronDownload?: unknown }).electronDownload;
if (!hasElectronDownload) {
    void ensureDownloadServiceWorker().catch((err) => {
        console.warn('[anyfs] download SW registration failed:', err);
    });
}

// We intentionally do NOT use <StrictMode> here. StrictMode double-invokes
// effects in dev, which would spawn two wasm Workers per disk mount — each
// boots its own LKL kernel and consumes ~64 MB of memory plus 32 pthread
// workers. Producing a clean second mount also doesn't exercise anything
// meaningful in this app: the wasm bridge is the heavy state, and it's
// owned by a dedicated worker outside the React tree.
createRoot(document.getElementById('root')!).render(<App />);
