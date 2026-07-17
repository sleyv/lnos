#!/usr/bin/env bash
set -e
REPO="https://github.com/sleyv/lnos.git"
DEST="${HOME}/lnos"
[ -d "$DEST" ] || git clone --depth=1 "$REPO" "$DEST"
cd "$DEST"
chmod +x setup.sh
exec ./setup.sh "$@"