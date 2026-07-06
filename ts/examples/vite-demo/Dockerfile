# Serves the pre-built vite-demo static dist. The build context must be the
# unpacked anyfs-web-dist tarball (produced by wasm.yml's web-package job),
# NOT a repo checkout — the wasm bundle + vite build happen upstream in CI.
#
# Local smoke (without CI): from ts/examples/vite-demo, after `pnpm build`:
#   docker build -t anyfs-web-local -f Dockerfile dist/
#   docker run --rm -p 4173:4173 anyfs-web-local
FROM caddy:2-alpine
COPY Caddyfile /etc/caddy/Caddyfile
COPY . /srv
EXPOSE 4173
