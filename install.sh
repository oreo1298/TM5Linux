#!/usr/bin/env bash
# TM5 for Linux - installer for every common distribution.
#
#   ./install.sh              install (asks for your password when needed)
#   ./install.sh --update     pull the latest version from git and reinstall
#   ./install.sh --uninstall  remove TM5 for Linux again
#   ./install.sh --help       all options
#
# SPDX-License-Identifier: MIT
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="/usr/local"
INSTALL_DEPS=1
SETCAP=1
ASSUME_YES=0
MODE="install"
CLI_ONLY=0

# ---- output helpers ------------------------------------------------------------------------------

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    B=$'\e[1m'; D=$'\e[2m'; R=$'\e[0m'; RED=$'\e[91m'; GRN=$'\e[92m'; YEL=$'\e[93m'; CYN=$'\e[96m'
else
    B=""; D=""; R=""; RED=""; GRN=""; YEL=""; CYN=""
fi
STEP=0
STEPS=6
step()  { STEP=$((STEP + 1)); printf '\n%s[%d/%d] %s%s\n' "$B$CYN" "$STEP" "$STEPS" "$1" "$R"; }
info()  { printf '  %s\n' "$*"; }
ok()    { printf '  %s✔%s %s\n' "$GRN" "$R" "$*"; }
warn()  { printf '  %s▲ %s%s\n' "$YEL" "$*" "$R"; }
fail()  { printf '\n%s✘ %s%s\n' "$RED$B" "$*" "$R" >&2; exit 1; }
run()   { printf '  %s$ %s%s\n' "$D" "$*" "$R"; "$@"; }

usage() {
    cat <<EOF
TM5 for Linux installer

Usage: ./install.sh [options]

  --prefix DIR     install location (default: /usr/local)
  --update         update an existing git checkout and reinstall
  --uninstall      remove TM5 for Linux (same as ./uninstall.sh)
  --no-deps        do not install dependencies with the package manager
  --no-setcap      do not allow memory locking for normal users
  --cli-only       skip the GUI dependencies (GTK 4, PyGObject)
  -y, --yes        do not ask questions
  -h, --help       show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)    [[ $# -ge 2 ]] || fail "--prefix needs a directory"; PREFIX="$2"; shift ;;
        --prefix=*)  PREFIX="${1#*=}" ;;
        --update)    MODE="update" ;;
        --uninstall) MODE="uninstall" ;;
        --no-deps)   INSTALL_DEPS=0 ;;
        --no-setcap) SETCAP=0 ;;
        --cli-only)  CLI_ONLY=1 ;;
        -y|--yes)    ASSUME_YES=1 ;;
        -h|--help)   usage; exit 0 ;;
        *)           usage >&2; fail "Unknown option: $1" ;;
    esac
    shift
done

if [[ "$MODE" == "uninstall" ]]; then
    args=(--prefix "$PREFIX")
    [[ $ASSUME_YES -eq 1 ]] && args+=(--yes)
    exec "$SRC_DIR/uninstall.sh" "${args[@]}"
fi

[[ "$PREFIX" == /* ]] || fail "--prefix must be an absolute path"
PREFIX="${PREFIX%/}"

# ---- privileges ------------------------------------------------------------------------------------

SUDO=()
if [[ $EUID -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO=(sudo)
    elif command -v doas >/dev/null 2>&1; then
        SUDO=(doas)
    else
        fail "Please run this script as root (neither sudo nor doas was found)."
    fi
fi
as_root() { if [[ ${#SUDO[@]} -gt 0 ]]; then run "${SUDO[@]}" "$@"; else run "$@"; fi; }
root_quiet() { if [[ ${#SUDO[@]} -gt 0 ]]; then "${SUDO[@]}" "$@"; else "$@"; fi; }

# Build as the normal user even if the script was started with sudo, so the source tree does not end
# up owned by root.
BUILD_USER=""
if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
    BUILD_USER="$SUDO_USER"
fi
as_user() {
    if [[ -n "$BUILD_USER" ]]; then run sudo -u "$BUILD_USER" -- "$@"; else run "$@"; fi
}

confirm() {
    [[ $ASSUME_YES -eq 1 || ! -t 0 ]] && return 0
    local reply
    read -r -p "  $1 [Y/n] " reply || true
    [[ -z "$reply" || "$reply" =~ ^[Yy] ]]
}

# ---- distribution detection ------------------------------------------------------------------------

DISTRO_NAME="Linux"
DISTRO_IDS=""
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_NAME="${PRETTY_NAME:-${NAME:-Linux}}"
    DISTRO_IDS="${ID:-} ${ID_LIKE:-}"
fi

PM=""
PKGS=()
pick_packages() {
    local gui=1
    [[ $CLI_ONLY -eq 1 ]] && gui=0
    for id in $DISTRO_IDS; do
        case "$id" in
            arch|archarm|manjaro|endeavouros|cachyos|garuda|artix|arcolinux|steamos)
                PM="pacman"; PKGS=(gcc make libcap)
                [[ $gui -eq 1 ]] && PKGS+=(python python-gobject gtk4)
                return ;;
            debian|ubuntu|linuxmint|pop|elementary|zorin|kali|raspbian|neon|deepin|mx|devuan|pureos)
                PM="apt"; PKGS=(gcc make libc6-dev libcap2-bin)
                [[ $gui -eq 1 ]] && PKGS+=(python3 python3-gi gir1.2-gtk-4.0)
                return ;;
            fedora|rhel|centos|rocky|almalinux|nobara|ultramarine|ol|amzn)
                PM="dnf"; PKGS=(gcc make libcap)
                [[ $gui -eq 1 ]] && PKGS+=(python3 python3-gobject gtk4)
                return ;;
            opensuse|opensuse-tumbleweed|opensuse-leap|opensuse-slowroll|suse|sles|sled)
                PM="zypper"; PKGS=(gcc make libcap-progs)
                [[ $gui -eq 1 ]] && PKGS+=(python3 python3-gobject python3-gobject-Gdk typelib-1_0-Gtk-4_0)
                return ;;
            void)
                PM="xbps"; PKGS=(gcc make libcap-progs)
                [[ $gui -eq 1 ]] && PKGS+=(python3 python3-gobject gtk4)
                return ;;
            gentoo|funtoo)
                PM="emerge"; PKGS=(sys-libs/libcap)
                [[ $gui -eq 1 ]] && PKGS+=(dev-python/pygobject gui-libs/gtk)
                return ;;
            solus)
                PM="eopkg"; PKGS=(gcc make libcap-progs)
                [[ $gui -eq 1 ]] && PKGS+=(python3 python3-gobject libgtk-4)
                return ;;
            alpine)
                PM="apk"; PKGS=(gcc make musl-dev linux-headers libcap-setcap)
                [[ $gui -eq 1 ]] && PKGS+=(python3 py3-gobject3 gtk4.0)
                return ;;
            nixos)
                PM="nix"; return ;;
        esac
    done
}

install_packages() {
    case "$PM" in
        pacman)
            local flags=(-S --needed)
            [[ $ASSUME_YES -eq 1 ]] && flags+=(--noconfirm)
            as_root pacman "${flags[@]}" "${PKGS[@]}" ;;
        apt)
            as_root apt-get update
            local flags=(install)
            [[ $ASSUME_YES -eq 1 ]] && flags+=(-y)
            as_root apt-get "${flags[@]}" "${PKGS[@]}" ;;
        dnf)
            local dnf=dnf
            command -v dnf >/dev/null 2>&1 || dnf=yum
            local flags=(install)
            [[ $ASSUME_YES -eq 1 ]] && flags+=(-y)
            as_root "$dnf" "${flags[@]}" "${PKGS[@]}" ;;
        zypper)
            local flags=()
            [[ $ASSUME_YES -eq 1 ]] && flags+=(--non-interactive)
            as_root zypper "${flags[@]}" install "${PKGS[@]}" ;;
        xbps)
            local flags=(-S)
            [[ $ASSUME_YES -eq 1 ]] && flags+=(-y)
            as_root xbps-install "${flags[@]}" "${PKGS[@]}" ;;
        emerge)
            as_root emerge --noreplace "${PKGS[@]}" ;;
        eopkg)
            local flags=(install)
            [[ $ASSUME_YES -eq 1 ]] && flags+=(-y)
            as_root eopkg "${flags[@]}" "${PKGS[@]}" ;;
        apk)
            as_root apk add "${PKGS[@]}" ;;
    esac
}

have_gui_deps() {
    local py
    for py in python3 python /usr/bin/python3 /usr/bin/python3.[0-9]*; do
        [[ "$py" == *-config ]] && continue
        if command -v "$py" >/dev/null 2>&1 &&
           "$py" -c 'import gi; gi.require_version("Gtk", "4.0"); from gi.repository import Gtk' >/dev/null 2>&1; then
            return 0
        fi
    done
    return 1
}

git_version() {
    if git -C "$SRC_DIR" rev-parse --git-dir >/dev/null 2>&1; then
        git -C "$SRC_DIR" describe --tags --always --dirty 2>/dev/null || true
    fi
}

# ---- update: pull first ---------------------------------------------------------------------------------

if [[ -z "${TM5L_UPDATING:-}" ]]; then
    printf '%sTM5 for Linux%s %s\n' "$B$CYN" "$R" "$( [[ $MODE == update ]] && echo updater || echo installer )"
fi

if [[ "$MODE" == "update" ]]; then
    printf '\n%s%s%s\n' "$B$CYN" "Downloading the latest version" "$R"
    git -C "$SRC_DIR" rev-parse --git-dir >/dev/null 2>&1 ||
        fail "This folder is not a git checkout. Download the latest version with:
    git clone https://github.com/oreo1298/TM5Linux.git && cd TM5Linux && ./install.sh"
    before="$(git_version)"
    as_user git -C "$SRC_DIR" pull --ff-only ||
        fail "git pull failed. If you changed files in this folder, run 'git stash' first and try again."
    after="$(git_version)"
    if [[ "$before" == "$after" ]]; then
        ok "Already up to date ($after) - reinstalling anyway."
    else
        ok "Updated from $before to $after."
    fi
    # Re-run the freshly downloaded installer so that changes to this script take effect too.
    args=(--prefix "$PREFIX")
    [[ $INSTALL_DEPS -eq 0 ]] && args+=(--no-deps)
    [[ $SETCAP -eq 0 ]] && args+=(--no-setcap)
    [[ $CLI_ONLY -eq 1 ]] && args+=(--cli-only)
    [[ $ASSUME_YES -eq 1 ]] && args+=(--yes)
    export TM5L_UPDATING=1
    exec "$SRC_DIR/install.sh" "${args[@]}"
fi

# ---- 1. check -------------------------------------------------------------------------------------------

step "Checking your system"
pick_packages
ok "Distribution: $DISTRO_NAME${PM:+ (package manager: $PM)}"
ok "Install location: $PREFIX"
MANIFEST="$PREFIX/share/tm5linux/install-manifest.txt"
if [[ -f "$MANIFEST" ]]; then
    info "An existing installation was found - it will be replaced."
fi
for pm_check in "pacman -Qo" "dpkg -S" "rpm -qf"; do
    tool="${pm_check%% *}"
    # shellcheck disable=SC2086  # word splitting of $pm_check is intended
    if command -v "$tool" >/dev/null 2>&1 && [[ -e /usr/bin/tm5linux-cli && "$PREFIX" != "/usr" ]] &&
       $pm_check /usr/bin/tm5linux-cli >/dev/null 2>&1; then
        warn "TM5 for Linux is also installed as a distribution package in /usr."
        warn "Two copies can be confusing - consider removing the package first."
        break
    fi
done

# ---- 2. dependencies -------------------------------------------------------------------------------------

step "Installing dependencies"
if [[ $INSTALL_DEPS -eq 0 ]]; then
    info "Skipped (--no-deps)."
elif [[ "$PM" == "nix" ]]; then
    warn "NixOS cannot install packages imperatively. Run this installer inside:"
    info "  nix-shell -p gcc gnumake libcap python3 python3Packages.pygobject3 gtk4 gobject-introspection"
elif [[ -z "$PM" ]]; then
    warn "Unknown distribution - please install these yourself if they are missing:"
    info "  a C compiler (gcc or clang), make, setcap (libcap), Python 3, PyGObject and GTK 4"
else
    info "Packages: ${PKGS[*]}"
    install_packages || fail "Installing the dependencies failed. Fix the problem above, or install them yourself and run: ./install.sh --no-deps"
    ok "Dependencies are installed."
fi
command -v make >/dev/null 2>&1 || fail "'make' is missing - please install it."
if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
    fail "No C compiler found - please install gcc or clang."
fi
CC_BIN="$(command -v cc || command -v gcc || command -v clang)"
GUI_OK=1
if ! have_gui_deps; then
    GUI_OK=0
    warn "GTK 4 for Python (PyGObject) was not found - the window version will not start,"
    warn "but the terminal version (tm5linux-cli) works."
fi

# ---- 3. build -----------------------------------------------------------------------------------------------

step "Building"
jobs="$(nproc 2>/dev/null || echo 2)"
as_user make -C "$SRC_DIR" -j"$jobs" CC="$CC_BIN" PREFIX="$PREFIX" >/dev/null
ok "Built $SRC_DIR/build/tm5linux-cli"

# ---- 4. self-test ---------------------------------------------------------------------------------------

step "Running a quick self-test"
if as_user "$SRC_DIR/build/tm5linux-cli" --selftest >/dev/null; then
    ok "The test engine finds every simulated memory error."
else
    fail "The self-test failed - please report this at https://github.com/oreo1298/TM5Linux/issues"
fi

# ---- 5. install -------------------------------------------------------------------------------------------

step "Installing to $PREFIX"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
make -C "$SRC_DIR" PREFIX="$PREFIX" DESTDIR="$STAGE" install >/dev/null
chmod -R u+rwX,go+rX "$STAGE"

if [[ -f "$MANIFEST" ]]; then
    # Remove files of the previous version that no longer exist.
    while IFS= read -r old; do
        [[ -z "$old" || "$old" == \#* ]] && continue
        [[ -e "$STAGE$old" ]] && continue
        [[ "$old" == "$PREFIX"/* ]] || continue
        if [[ -e "$old" || -L "$old" ]]; then root_quiet rm -f -- "$old"; fi
    done < "$MANIFEST"
fi

MANIFEST_TMP="$STAGE.manifest"
{
    echo "# TM5 for Linux install manifest - used by uninstall.sh"
    echo "# version: $(git_version || true) installed: $(date '+%Y-%m-%d %H:%M:%S') prefix: $PREFIX"
    (cd "$STAGE" && find . -type f | sed 's|^\.||' | sort)
    echo "$MANIFEST"
} > "$MANIFEST_TMP"

count=0
while IFS= read -r rel; do
    mode="$(stat -c '%a' "$STAGE$rel")"
    root_quiet install -D -m "$mode" "$STAGE$rel" "$rel"
    count=$((count + 1))
done < <(cd "$STAGE" && find . -type f | sed 's|^\.||' | sort)
root_quiet install -D -m 644 "$MANIFEST_TMP" "$MANIFEST"
rm -f "$MANIFEST_TMP"
ok "Installed $count files."

# Refresh menus and icons (not every desktop needs this, so failures are fine).
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [[ -f "$PREFIX/share/icons/hicolor/index.theme" ]]; then
    root_quiet gtk-update-icon-cache -q -t -f "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    root_quiet update-desktop-database -q "$PREFIX/share/applications" >/dev/null 2>&1 || true
fi

# ---- 6. memory locking -------------------------------------------------------------------------------------

step "Allowing memory locking"
CLI="$PREFIX/bin/tm5linux-cli"
if [[ $SETCAP -eq 0 ]]; then
    info "Skipped (--no-setcap). Tests will run without locked memory unless started as root."
elif ! command -v setcap >/dev/null 2>&1 && [[ ! -x /usr/sbin/setcap && ! -x /sbin/setcap ]]; then
    warn "'setcap' is not installed (package libcap / libcap2-bin / libcap-progs)."
    warn "Install it and run: sudo setcap cap_ipc_lock=ep $CLI"
else
    info "This lets the test keep the tested memory in RAM (CAP_IPC_LOCK) without running as root."
    if confirm "Allow TM5 for Linux to lock memory?"; then
        SETCAP_BIN="$(command -v setcap || echo /usr/sbin/setcap)"
        if as_root "$SETCAP_BIN" cap_ipc_lock=ep "$CLI"; then
            ok "Memory locking is allowed."
        else
            warn "setcap failed (some file systems do not support capabilities)."
            warn "You can still tick \"Run with administrator rights\" in the app."
        fi
    else
        info "Skipped. You can do it later with: sudo setcap cap_ipc_lock=ep $CLI"
    fi
fi

# ---- done -----------------------------------------------------------------------------------------------------

case ":$PATH:" in
    *":$PREFIX/bin:"*) on_path=1 ;;
    *) on_path=0 ;;
esac

done_msg="TM5 for Linux is installed."
[[ -n "${TM5L_UPDATING:-}" ]] && done_msg="TM5 for Linux is updated and installed."
printf '\n%s✔ %s%s\n\n' "$GRN$B" "$done_msg" "$R"
if [[ $GUI_OK -eq 1 ]]; then
    info "Start it from your applications menu (${B}TM5 for Linux${R}) or run:  ${B}tm5linux${R}"
fi
info "Terminal version:   ${B}tm5linux-cli${R}   (see tm5linux-cli --help)"
info "Update later:       ${B}./update.sh${R}"
info "Uninstall:          ${B}./uninstall.sh${R}   or   ${B}sudo $PREFIX/share/tm5linux/uninstall.sh${R}"
if [[ $on_path -eq 0 ]]; then
    warn "$PREFIX/bin is not in your PATH - add it, or start the programs with their full path."
fi
echo
