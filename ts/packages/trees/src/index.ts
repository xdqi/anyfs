export { AnyfsFileBrowser } from './AnyfsFileBrowser';
export type { AnyfsFileBrowserProps } from './AnyfsFileBrowser';

// Back-compat alias for the old @pierre/trees-based component name. The new
// component takes the same `disk` / `mountPath` / `className` / `onFileActivate`
// props, so most call sites only need a rename.
export { AnyfsFileBrowser as FileTreeView } from './AnyfsFileBrowser';
export type { AnyfsFileBrowserProps as FileTreeViewProps } from './AnyfsFileBrowser';
