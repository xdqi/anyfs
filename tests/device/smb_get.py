#!/usr/bin/env python3
"""Fetch files from an SMB share with the smbprotocol package (pure Python).

Usage: smb_get.py HOST PORT SHARE REMOTE LOCAL [REMOTE LOCAL ...]

The SMB client of tests/device/run-cli-device-tests.sh where no system client
can reach anyfs-ksmbd: Windows' own client cannot be pointed at a non-445 port
with guest access. Logs in as guest/guest without signing, as smbclient does
with -N against anyfs-ksmbd (so without the secure-negotiate check, which
needs a signing key). Exit status 0 when every file was fetched.
Needs: python -m pip install smbprotocol
"""
import sys

import smbclient


def main():
    if len(sys.argv) < 6 or (len(sys.argv) - 4) % 2:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    host, port, share = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    # A guest session has no signing key, so the secure-negotiate check of
    # the tree connect cannot run.
    smbclient.ClientConfig(require_secure_negotiate=False)
    smbclient.register_session(host, username="guest", password="guest",
                               port=port, require_signing=False)
    pairs = sys.argv[4:]
    for remote, local in zip(pairs[0::2], pairs[1::2]):
        path = "\\\\%s\\%s\\%s" % (host, share, remote.replace("/", "\\"))
        with smbclient.open_file(path, mode="rb", port=port) as src, \
                open(local, "wb") as dst:
            while True:
                chunk = src.read(1 << 20)
                if not chunk:
                    break
                dst.write(chunk)
    return 0


if __name__ == "__main__":
    sys.exit(main())
