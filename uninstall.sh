#!/usr/bin/env bash
# TM5 for Linux - uninstaller.
#
#   ./uninstall.sh              remove TM5 for Linux (keeps your logs and own configs)
#   ./uninstall.sh --purge      also remove your logs, settings and own configs
#
# A copy of this script is installed as <prefix>/share/tm5linux/uninstall.sh, so you can uninstall
# even after deleting the downloaded folder:  sudo /usr/local/share/tm5linux/uninstall.sh
#
# SPDX-License-Identifier: MIT
set -euo pipefail

PREFIX=""
ASSUME_YES=0
PURGE=0
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    B=$'\e[1m'; D=$'\e[2m'; R=$'\e[0m'; RED=$'\e[91m'; GRN=$'\e[92m'; YEL=$'\e[93m'; CYN=$'\e[96m'
else
    B=""; D=""; R=""; RED=""; GRN=""; YEL=""; CYN=""
fi
info() { printf '  %s\n' "$*"; }
ok()   { printf '  %s✔%s %s\n' "$GRN" "$R" "$*"; }
warn() { printf '  %s▲ %s%s\n' "$YEL" "$*" "$R"; }
fail() { printf '\n%s✘ %s%s\n' "$RED$B" "$*" "$R" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)   [[ $# -ge 2 ]] || fail "--prefix needs a directory"; PREFIX="${2%/}"; shift ;;
        --prefix=*) PREFIX="${1#*=}"; PREFIX="${PREFIX%/}" ;;
        --purge)    PURGE=1 ;;
        -y|--yes)   ASSUME_YES=1 ;;
        -h|--help)
            cat <<EOF
TM5 for Linux uninstaller

Usage: ./uninstall.sh [--prefix DIR] [--purge] [--yes]

  --prefix DIR   where TM5 for Linux was installed (found automatically)
  --purge        also delete your logs, settings and own configs
  -y, --yes      do not ask questions
EOF
            exit 0 ;;
        *) fail "Unknown option: $1 (see --help)" ;;
    esac
    shift
done

printf '%sTM5 for Linux%s uninstaller\n\n' "$B$CYN" "$R"

SUDO=()
if [[ $EUID -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then SUDO=(sudo)
    elif command -v doas >/dev/null 2>&1; then SUDO=(doas)
    fi
fi
root_quiet() { if [[ ${#SUDO[@]} -gt 0 ]]; then "${SUDO[@]}" "$@"; else "$@"; fi; }

confirm() {
    [[ $ASSUME_YES -eq 1 || ! -t 0 ]] && return 0
    local reply
    read -r -p "  $1 [y/N] " reply || true
    [[ "$reply" =~ ^[Yy] ]]
}

# ---- find the installation ------------------------------------------------------------------------------

MANIFEST=""
candidates=()
[[ -n "$PREFIX" ]] && candidates+=("$PREFIX/share/tm5linux/install-manifest.txt")
candidates+=("$SELF_DIR/install-manifest.txt" /usr/local/share/tm5linux/install-manifest.txt
             /usr/share/tm5linux/install-manifest.txt "$HOME/.local/share/tm5linux/install-manifest.txt")
for m in "${candidates[@]}"; do
    if [[ -f "$m" ]]; then MANIFEST="$m"; break; fi
done

if [[ -z "$MANIFEST" ]]; then
    # Installed through a distribution package?
    if [[ -e /usr/bin/tm5linux-cli ]]; then
        if command -v pacman >/dev/null 2>&1 && pacman -Qo /usr/bin/tm5linux-cli >/dev/null 2>&1; then
            pkg="$(pacman -Qqo /usr/bin/tm5linux-cli)"
            fail "TM5 for Linux was installed with pacman. Remove it with:  sudo pacman -Rns $pkg"
        elif command -v dpkg >/dev/null 2>&1 && dpkg -S /usr/bin/tm5linux-cli >/dev/null 2>&1; then
            fail "TM5 for Linux was installed as a .deb package. Remove it with:  sudo apt remove tm5linux"
        elif command -v rpm >/dev/null 2>&1 && rpm -qf /usr/bin/tm5linux-cli >/dev/null 2>&1; then
            fail "TM5 for Linux was installed as an RPM package. Remove it with:  sudo dnf remove tm5linux  (or zypper remove tm5linux)"
        fi
    fi
    fail "No installation of TM5 for Linux was found${PREFIX:+ in $PREFIX}.
  If you installed it somewhere else, pass the location:  ./uninstall.sh --prefix /path"
fi

header="$(sed -n '2p' "$MANIFEST")"
if [[ -z "$PREFIX" ]]; then
    PREFIX="$(sed -n 's/.*prefix: //p' <<<"$header")"
    [[ -n "$PREFIX" ]] || PREFIX="$(dirname "$(dirname "$(dirname "$MANIFEST")")")"
fi
[[ "$PREFIX" == /* && "$PREFIX" != "/" ]] || fail "Refusing to uninstall from '$PREFIX'."

mapfile -t FILES < <(grep -v '^#' "$MANIFEST" | grep -v '^$')
info "Found TM5 for Linux in ${B}$PREFIX${R} (${#FILES[@]} files)."
info "${D}${header#\# }${R}"

if [[ $EUID -ne 0 && ${#SUDO[@]} -eq 0 && ! -w "$PREFIX/bin" ]]; then
    fail "Root rights are needed. Run:  su -c '$0 --prefix $PREFIX'"
fi

if ! confirm "Remove TM5 for Linux from $PREFIX?"; then
    info "Nothing was changed."
    exit 0
fi

# ---- remove ---------------------------------------------------------------------------------------------

removed=0
for f in "${FILES[@]}"; do
    case "$f" in
        "$PREFIX"/*) ;;
        *) warn "Skipping $f (outside $PREFIX)"; continue ;;
    esac
    if [[ -f "$f" || -L "$f" ]]; then
        root_quiet rm -f -- "$f"
        removed=$((removed + 1))
    fi
done
for d in "$PREFIX/share/tm5linux" "$PREFIX/share/doc/tm5linux"; do
    [[ -d "$d" ]] && root_quiet rm -rf -- "$d"
done
ok "Removed $removed files."

if command -v gtk-update-icon-cache >/dev/null 2>&1 && [[ -f "$PREFIX/share/icons/hicolor/index.theme" ]]; then
    root_quiet gtk-update-icon-cache -q -t -f "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1 && [[ -d "$PREFIX/share/applications" ]]; then
    root_quiet update-desktop-database -q "$PREFIX/share/applications" >/dev/null 2>&1 || true
fi

# ---- personal data ----------------------------------------------------------------------------------------

USER_HOME="$HOME"
if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" ]]; then
    USER_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
fi
data=("$USER_HOME/.config/tm5linux" "$USER_HOME/.local/share/tm5linux")
existing=()
for d in "${data[@]}"; do [[ -d "$d" ]] && existing+=("$d"); done

if [[ ${#existing[@]} -gt 0 ]]; then
    if [[ $PURGE -eq 1 ]]; then
        rm -rf -- "${existing[@]}"
        ok "Removed your logs, settings and own configs."
    else
        info "Your logs, settings and own configs were kept:"
        for d in "${existing[@]}"; do info "  $d"; done
        info "Delete them too with:  ./uninstall.sh --purge   (or remove those folders yourself)"
    fi
fi

printf '\n%s✔ TM5 for Linux has been removed.%s\n\n' "$GRN$B" "$R"
