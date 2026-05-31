/*
 * Parent/launcher for the NBD-over-fd PoC.
 *
 * Two entry points:
 *   serveOnFd(imagePath): create a socketpair, run the NBD server on fd0,
 *     return fd1 (the inheritable end) + a stop() handle and read counter.
 *   serveOnUnixSocket(imagePath, sockPath): listen on a unix socket and
 *     serve clients — used to cross-validate against qemu-img / qemu-io.
 */
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { startNbdServer } from './nbd-server.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const addon = require(
  path.join(here, 'native-transport/build/Release/native_transport.node'),
);

/* The NBD export size is the RAW byte length of the file on disk. QEMU's
 * NBD client reads those raw bytes and probes the qcow2 header itself, so
 * the server must serve the file verbatim (not the qcow2 virtual size). */
function imageSize(imagePath) {
  return fs.statSync(imagePath).size;
}

export function serveOnFd(imagePath) {
  const [fd0, fd1] = addon.socketpair();
  const imageFd = fs.openSync(imagePath, 'r');
  const size = imageSize(imagePath);
  const sock = new net.Socket({ fd: fd0 });
  let reads = 0;
  const done = startNbdServer(sock, imageFd, size, () => {
    reads++;
  }).catch((e) => {
    if (!/EOF/.test(String(e))) console.error('[nbd-server]', e);
  });
  return {
    fd1,
    stop: () => {
      try {
        sock.destroy();
      } catch {}
      try {
        fs.closeSync(imageFd);
      } catch {}
    },
    readCount: () => reads,
    done,
  };
}

export function serveOnUnixSocket(imagePath, sockPath) {
  const imageFd = fs.openSync(imagePath, 'r');
  const size = imageSize(imagePath);
  try {
    fs.unlinkSync(sockPath);
  } catch {}
  const server = net.createServer((sock) => {
    startNbdServer(sock, imageFd, size).catch((e) => {
      if (!/EOF/.test(String(e))) console.error('[nbd-server]', e);
    });
  });
  return new Promise((resolve) => {
    server.listen(sockPath, () => resolve({ server, imageFd }));
  });
}
