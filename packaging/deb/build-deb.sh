#!/usr/bin/env bash
# Build a .deb package for Debian, Ubuntu, Linux Mint, Pop!_OS and friends.
#
#   ./packaging/deb/build-deb.sh
#   sudo apt install ./tm5linux_*.deb        install / update
#   sudo apt remove tm5linux                 uninstall
#
# Needs: gcc make dpkg-dev   (sudo apt install build-essential)
# SPDX-License-Identifier: MIT
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$(sed -n 's/^VERSION *:= *//p' "$ROOT/Makefile")"
ARCH="$(dpkg --print-architecture)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

make -C "$ROOT" clean >/dev/null
make -C "$ROOT" PREFIX=/usr -j"$(nproc)"
"$ROOT/build/tm5linux-cli" --selftest
make -C "$ROOT" PREFIX=/usr DESTDIR="$STAGE" install >/dev/null
rm -f "$STAGE/usr/share/tm5linux/uninstall.sh"
gzip -9n "$STAGE/usr/share/man/man1/tm5linux-cli.1"
install -Dm644 "$ROOT/LICENSE" "$STAGE/usr/share/doc/tm5linux/copyright"

SIZE_KB="$(du -sk "$STAGE/usr" | cut -f1)"
mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: tm5linux
Version: $VERSION
Architecture: $ARCH
Maintainer: TM5 for Linux contributors <tm5linux@users.noreply.github.com>
Installed-Size: $SIZE_KB
Depends: libc6, python3, python3-gi, gir1.2-gtk-4.0, libcap2-bin
Recommends: pkexec | policykit-1
Section: utils
Priority: optional
Homepage: https://github.com/oreo1298/TM5Linux
Description: RAM stability tester that runs TestMem5 (TM5) configs
 TM5 for Linux tests memory stability like TestMem5 on Windows. It reads
 TM5 config files directly and ships the Extreme1 profile by anta777,
 ABSOLUT, Universal2 and PCBDestroyer. Comes with a dark GTK 4 interface
 (tm5linux) and a terminal version (tm5linux-cli).
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    if command -v setcap >/dev/null 2>&1; then
        setcap cap_ipc_lock=ep /usr/bin/tm5linux-cli || \
            echo "TM5 for Linux: could not set CAP_IPC_LOCK; memory locking needs root." >&2
    fi
fi
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/postinst"

OUT="$ROOT/tm5linux_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
echo
echo "Built $OUT"
echo "Install it with:  sudo apt install $OUT"
