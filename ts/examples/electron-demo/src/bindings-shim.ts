/*
 * Stand-in for the `bindings` npm package, baked into the main-process
 * bundle so drivelist's `require('bindings')` resolves to a known path
 * instead of crawling __dirname (which after bundling lives in the
 * Electron asar/dist tree, far from the staged .node file).
 *
 * Only drivelist uses bindings in our codebase; if another consumer
 * appears, extend the switch below.
 */
import { resolveDrivelistNode } from './native-loader';

function bindings(name: string): unknown {
    if (name === 'drivelist' || name === 'drivelist.node') {
        const p = resolveDrivelistNode();
        if (!p) throw new Error('drivelist .node not found at any staged path');
        return require(p);
    }
    throw new Error(`bindings-shim: unsupported binding '${name}'`);
}

export default bindings;
