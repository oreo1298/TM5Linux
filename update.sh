#!/usr/bin/env bash
# TM5 for Linux - update to the latest version (git pull + rebuild + reinstall).
# Same options as install.sh, e.g.  ./update.sh --yes
# SPDX-License-Identifier: MIT
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/install.sh" --update "$@"
