#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Install missing build dependencies, build and install Way Vibrance, then
# load the effect into the running KWin session.
#
# Distro families with dependency mappings: Arch (and derivatives),
# Fedora/RHEL-like, and Debian/Ubuntu-like — KDE Plasma 6 era required
# (Fedora 40+, Debian 13+, Ubuntu 24.10+, KDE Neon, Mint 22+). Unknown
# families skip the dependency step and build anyway if the development
# packages are already present.
#
# Never upgrades the system: existing packages are left exactly as they are.
# For OSTree-based immutable distros (Silverblue, SteamOS…) see the note at
# the bottom of this file.

# Refuse the destructive side by default when running inside a container:
# building is the supported flow there (see the immutable note at the
# bottom). Set DESTDIR (proper packaging) or ALLOW_CONTAINER_INSTALL=1 to
# install inside the image anyway.
INSTALL=1
if { [ -f /run/.containerenv ] || [ -f /.dockerenv ]; } \
   && [ -z "${DESTDIR:-}" ] && [ "${ALLOW_CONTAINER_INSTALL:-0}" != 1 ] \
   && [ "$(id -u)" -eq 0 ] && [ "${PREFIX:-/usr}" = "/usr" ]; then
    echo "==> Running inside a container: not installing into the image's /usr (build only)." >&2
    echo "    On the host install normally; for packaging set DESTDIR; for an" >&2
    echo "    intentional in-image install re-run with ALLOW_CONTAINER_INSTALL=1." >&2
    INSTALL=0
fi

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
PREFIX="${PREFIX:-/usr}"

if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
else
    SUDO=""   # dependency steps will fail loudly if privileges are truly needed
fi

detect_family() {
    [ -r /etc/os-release ] || return 1
    local ID="" ID_LIKE="" tok
    # shellcheck disable=SC1091,SC1090
    . /etc/os-release
    for tok in $ID $ID_LIKE; do
        case "$tok" in
            arch|manjaro|cachyos|endeavouros|garuda) echo arch; return ;;
            fedora|rhel|centos) echo fedora; return ;;
            *suse*|sles) echo suse; return ;;
            ubuntu|debian|linuxmint|pop|neon|deepin|uos) echo debian; return ;;
        esac
    done
    return 1
}

# Prints the first candidate package that is installed; empty when none.
# Always returns 0: callers run under set -e and "not installed" is a
# normal outcome, signalled by the empty result.
installed_pkg() { # <family> <candidate>...
    local family="$1"; shift
    local c
    for c in "$@"; do
        case "$family" in
            arch)   pacman -Qi "$c" >/dev/null 2>&1 && { echo "$c"; return 0; } ;;
            fedora) rpm -q "$c"   >/dev/null 2>&1 && { echo "$c"; return 0; } ;;
            debian) dpkg -s "$c"  >/dev/null 2>&1 && { echo "$c"; return 0; } ;;
        esac
    done
    return 0
}

# For each "label:candidate candidate…" line: queue the first candidate of
# every missing line for installation, and keep the full candidate list
# (CANDIDATES) so per-dependency fallback can try the alternatives.
collect_missing() { # <family> <spec-lines>...
    local family="$1"; shift
    local entry cands found
    for entry in "$@"; do
        cands=${entry#*:}
        found="$(installed_pkg "$family" $cands)"
        if [ -z "$found" ]; then
            MISSING+=("${cands%% *}")
            CANDIDATES+=("$cands")
        fi
    done
}

FAMILY="$(detect_family || true)"
declare -a MISSING=()
declare -a CANDIDATES=()

case "$FAMILY" in
    arch)
        echo "==> Checking build dependencies (Arch family)…"
        command -v make >/dev/null 2>&1 || MISSING+=(base-devel)
        collect_missing arch \
            "cmake:cmake" \
            "ecm:extra-cmake-modules" \
            "kwin:kwin" \
            "qt6:qt6-base" \
            "qttools:qt6-tools" \
            "kconfig:kconfig" \
            "kcoreaddons:kcoreaddons" \
            "kdbusaddons:kdbusaddons" \
            "kglobalaccel:kglobalaccel" \
            "vulkan:vulkan-headers" \
            "libdrm:libdrm" \
            "wayland:wayland"
        if ((${#MISSING[@]})); then
            # pacman -S resolves from the sync databases; sync the databases
            # (never the packages) only if they have not been fetched yet.
            [ -n "$(ls /var/lib/pacman/sync/*.db 2>/dev/null)" ] \
                || $SUDO pacman -Sy --noconfirm
            echo "==> Installing missing packages: ${MISSING[*]}"
            $SUDO pacman -S --needed --noconfirm "${MISSING[@]}"
        else
            echo "==> Build dependencies already present; nothing to install."
        fi
        ;;
    fedora)
        echo "==> Checking build dependencies (Fedora/RHEL family)…"
        command -v make >/dev/null 2>&1 || MISSING+=(make)
        command -v g++  >/dev/null 2>&1 || MISSING+=(gcc-c++)
        collect_missing fedora \
            "cmake:cmake" \
            "ecm:extra-cmake-modules" \
            "kwin:kwin-devel" \
            "qt6:qt6-qtbase-devel" \
            "qttools:qt6-qttools" \
            "kconfig:kf6-kconfig-devel" \
            "kcoreaddons:kf6-kcoreaddons-devel" \
            "kdbusaddons:kf6-kdbusaddons-devel" \
            "kglobalaccel:kf6-kglobalaccel-devel" \
            "epoxy:libepoxy-devel" \
            "vulkan:vulkan-headers" \
            "libdrm:libdrm-devel" \
            "wayland:wayland-devel"
        if ((${#MISSING[@]})); then
            echo "==> Installing missing packages: ${MISSING[*]}"
            $SUDO dnf install -y "${MISSING[@]}"
        else
            echo "==> Build dependencies already present; nothing to install."
        fi
        ;;
    debian)
        echo "==> Checking build dependencies (Debian/Ubuntu family)…"
        command -v make >/dev/null 2>&1 || { MISSING+=(make); CANDIDATES+=("make"); }
        command -v g++  >/dev/null 2>&1 || { MISSING+=(g++); CANDIDATES+=("g++ build-essential"); }
        collect_missing debian \
            "cmake:cmake" \
            "ecm:extra-cmake-modules" \
            "kwin:kwin-dev libkwin-dev libkwin-electron-dev" \
            "qt6:qt6-base-dev" \
            "qttools:qdbus-qt6 qt6-tools-bin" \
            "kconfig:libkf6config-dev" \
            "kcoreaddons:libkf6coreaddons-dev" \
            "kdbusaddons:libkf6dbusaddons-dev" \
            "kglobalaccel:libkf6globalaccel-dev" \
            "epoxy:libepoxy-dev" \
            "vulkan:libvulkan-dev" \
            "libdrm:libdrm-dev" \
            "wayland:libwayland-dev" \
            "pkgcfg:pkg-config"
        if ((${#MISSING[@]})); then
            # Pass 1: batch the first candidates — the normal path on
            # healthy systems.
            echo "==> Installing missing packages: ${MISSING[*]}"
            if ! $SUDO apt-get install -y --no-install-recommends "${MISSING[@]}" >/dev/null 2>&1; then
                # Pass 2: almost always an empty/stale apt list (containers,
                # fresh installs): refresh metadata once — apt-get update
                # changes no packages — and retry the batch.
                echo "==> Package lists stale; refreshing them (apt-get update — no system upgrade)…"
                $SUDO apt-get update
                if ! $SUDO apt-get install -y --no-install-recommends "${MISSING[@]}" >/dev/null 2>&1; then
                    # Pass 3: some name is genuinely unknown on this
                    # derivative: resolve each logical dependency
                    # individually against the cache.
                    echo "==> Some names unknown; resolving per dependency…"
                    apt_known() { apt-cache policy "$1" 2>/dev/null | grep -q "Candidate: [0-9]"; }
                    RESOLVED=()
                    for entry in "${CANDIDATES[@]}"; do
                        found=""
                        for c in $entry; do
                            if apt_known "$c"; then found="$c"; break; fi
                        done
                        if [ -n "$found" ]; then
                            RESOLVED+=("$found")
                        else
                            echo "    (nothing known for: $entry — the sanity check below will explain)" >&2
                        fi
                    done
                    if ((${#RESOLVED[@]} == 0)); then
                        echo "==> apt cannot resolve any dependency — check your sources configuration." >&2
                        exit 1
                    fi
                    echo "==> Installing resolvable packages: ${RESOLVED[*]}"
                    $SUDO apt-get install -y --no-install-recommends "${RESOLVED[@]}"
                fi
            fi
        else
            echo "==> Build dependencies already present; nothing to install."
        fi
        ;;
    suse)
        echo "==> openSUSE mapping is not bundled yet; install the KWin/Qt6/KF6 dev packages via zypper and re-run." >&2
        ;;
    *)
        echo "==> Unknown distro family; skipping the dependency step (development packages must already be installed)."
        ;;
esac

# The KWin effects headers are the hard build requirement: fail early and
# plainly instead of deep inside cmake.
if ! ls /usr/include/kwin/effect/effect.h >/dev/null 2>&1; then
    echo "==> KWin development headers not found. Install the kwin dev package:" >&2
    echo "    Arch: kwin · Fedora: kwin-devel · Debian/Ubuntu: libkwin-electron-dev (or libkwin-dev)" >&2
    exit 1
fi

echo "==> Configuring…"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"

echo "==> Building…"
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

if [ "$INSTALL" = 1 ]; then
    echo "==> Installing…"
    $SUDO cmake --install "$BUILD_DIR"
fi

echo "==> Loading the effect into KWin…"
QDBUS_BIN="$(command -v qdbus6 || command -v qdbus-qt6 || command -v qdbus || true)"
if [ -n "$QDBUS_BIN" ]; then
    "$QDBUS_BIN" org.kde.KWin /Effects org.kde.kwin.Effects.loadEffect wayvibrance 2>/dev/null \
        || "$QDBUS_BIN" org.kde.kwin.Effects /Effects org.kde.kwin.Effects.loadEffect wayvibrance 2>/dev/null \
        || echo "    (not running inside a Plasma session: the tray will load the effect via D-Bus on first start)"
else
    echo "    (no qdbus tool found; starting the tray loads the effect automatically)"
fi

echo "==> Done. Start the tray with: wayvibrance-tray"
echo "    (or enable 'Start with Plasma' in its tray menu)"

# ---------------------------------------------------------------------------
# Immutable distros (Fedora Silverblue/Kinoite, SteamOS, openSUSE MicroOS):
# dnf/rpm-ostree cannot layer dev packages on the host. Recipe that works:
#   1. distrobox enter (or podman run fedora:latest with the repo mounted)
#   2. ./scripts/build.sh with PREFIX set
#   3. copy the built wayvibrance.so to ~/.local/share/kwin/effects/plugins
#      and the tray/desktop entry to ~/.local/bin / ~/.local/share/applications
# User-scope loading support from KWin is being tracked on the roadmap
# (KWin currently prefers the system plugin directory).
