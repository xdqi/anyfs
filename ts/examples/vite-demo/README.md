# vite-demo

```
cd ts
pnpm install
pnpm --filter vite-demo dev
```

Then open <http://localhost:5173>. Drop one of:

- `lklftpd/disk_single.img` → auto-mount as ext4 → tree
- `lklftpd/disk_multi.img` → partition selector → pick one → tree
- `lklftpd/build/diagnostic/big_ext4.img` → 200 MB file inside ext4

Note: the dev server sets `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` because `-pthread` wasm needs
`SharedArrayBuffer`, which is only available in a cross-origin-isolated context.
