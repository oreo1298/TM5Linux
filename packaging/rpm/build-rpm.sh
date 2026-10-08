#!/usr/bin/env bash
# Build an RPM package for Fedora, RHEL/Rocky/Alma, openSUSE and friends.
#
#   ./packaging/rpm/build-rpm.sh
#   sudo dnf install ~/rpmbuild/RPMS/*/tm5linux-*.rpm          (openSUSE: sudo zypper install …)
#   sudo dnf remove tm5linux                                  uninstall
#
# Needs: rpm-build gcc make   (Fedora: sudo dnf install rpm-build gcc make)
# SPDX-License-Identifier: MIT
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$(sed -n 's/^VERSION *:= *//p' "$ROOT/Makefile")"
TOP="${RPM_TOPDIR:-$HOME/rpmbuild}"
mkdir -p "$TOP"/{SOURCES,SPECS,BUILD,RPMS,SRPMS}

TARBALL="$TOP/SOURCES/tm5linux-$VERSION.tar.gz"
if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    git -C "$ROOT" archive --format=tar.gz --prefix="tm5linux-$VERSION/" -o "$TARBALL" HEAD
else
    tar --exclude=./build --exclude=./.git -czf "$TARBALL" -C "$ROOT" --transform "s,^\.,tm5linux-$VERSION," .
fi
cp "$ROOT/packaging/rpm/tm5linux.spec" "$TOP/SPECS/"
rpmbuild --define "_topdir $TOP" -ba "$TOP/SPECS/tm5linux.spec"
echo
echo "Built packages:"
find "$TOP/RPMS" -name "tm5linux-$VERSION-*.rpm" -print
