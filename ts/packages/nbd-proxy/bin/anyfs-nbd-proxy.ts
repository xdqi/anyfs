#!/usr/bin/env node
import { createDataSource, type DataSourceSpec } from '../src/data-source.js';
import { serveOnFd, serveOnLoopback } from '../src/endpoint.js';

interface Args {
  source: 'file' | 'blockdev' | 'url' | undefined;
  target: string | undefined;
  fd: number | undefined;
  port: number | undefined;
}

function emptyArgs(): Args {
  return { source: undefined, target: undefined, fd: undefined, port: undefined };
}

function parse(argv: string[]): Args {
  const a = emptyArgs();
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const v = argv[i + 1];
    switch (k) {
      case '--source':
        a.source = v as Args['source'];
        i++;
        break;
      case '--target':
        a.target = v;
        i++;
        break;
      case '--fd':
        a.fd = Number(v);
        i++;
        break;
      case '--port':
        a.port = Number(v);
        i++;
        break;
      case '--help':
      case '-h':
        usage();
        process.exit(0);
    }
  }
  return a;
}

function usage(): void {
  process.stderr.write(
    'Usage: anyfs-nbd-proxy --source file|blockdev|url --target <path|/dev/sdX|url> ' +
      '(--fd <N> | --port <P>)\n',
  );
}

async function main(): Promise<void> {
  const a = parse(process.argv.slice(2));
  if (!a.source || !a.target || (a.fd === undefined && a.port === undefined)) {
    usage();
    process.exit(2);
  }
  const source = await createDataSource({
    kind: a.source,
    target: a.target,
  } as DataSourceSpec);

  if (a.fd !== undefined) {
    await serveOnFd(a.fd, source);
    await source.close();
    return;
  }
  const { port, stop } = await serveOnLoopback(source, a.port);
  process.stdout.write(`${port}\n`); /* report the bound port on stdout */
  const shutdown = async () => {
    await stop();
    await source.close();
    process.exit(0);
  };
  process.on('SIGTERM', shutdown);
  process.on('SIGINT', shutdown);
  /* Lifecycle binding: exit when stdin closes (parent gone). */
  process.stdin.on('end', shutdown);
  process.stdin.resume();
}

main().catch((e) => {
  process.stderr.write(`anyfs-nbd-proxy: ${e instanceof Error ? e.message : String(e)}\n`);
  process.exit(1);
});
