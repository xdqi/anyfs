import net from 'node:net';
import type { DataSource } from './data-source.js';
import { serveNbd } from './nbd-server.js';

/** Serve NBD on an already-open socket fd (inherited from the parent). */
export async function serveOnFd(fd: number, source: DataSource): Promise<void> {
  const size = await source.size();
  const socket = new net.Socket({ fd });
  return serveNbd(socket, source, { size });
}

/**
 * Serve NBD on a 127.0.0.1 loopback listener. Resolves with the bound port
 * and a stop() that closes the listener. Each connection is served from the
 * same source.
 */
export async function serveOnLoopback(
  source: DataSource,
  port = 0,
): Promise<{ port: number; stop: () => Promise<void> }> {
  const size = await source.size();
  const server = net.createServer((sock) => {
    serveNbd(sock, source, { size }).catch(() => {});
  });
  await new Promise<void>((r) => server.listen(port, '127.0.0.1', r));
  const addr = server.address() as net.AddressInfo;
  return {
    port: addr.port,
    stop: () => new Promise<void>((r) => server.close(() => r())),
  };
}
