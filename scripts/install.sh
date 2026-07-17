#!/usr/bin/env bash
set -e
exec < /dev/tty 2>/dev/null || true
REPO="https://github.com/sleyv/lnos.git"
DEST="${HOME}/lnos"
[ -d "$DEST" ] || git clone --depth=1 "$REPO" "$DEST"
cd "$DEST"
chmod +x setup.sh
exec ./setup.sh "$@"