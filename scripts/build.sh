#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Install missing build dependencies (Arch Linux / CachyOS), build and
# install Way Vibrance, then load the effect into the running KWin session.
# Never upgrades the system: existing packages are left exactly as they are.

set -euo pipefail
cd "$(dirname "$0")/.."

if command -v pacman >/dev/null 2>&1; then
    packages=(cmake extra-cmake-modules kwin qt6-base qt6-tools kconfig
              kcoreaddons kdbusaddons kglobalaccel vulkan-headers libdrm
              wayland)
    missing=()
    for pkg in "${packages[@]}"; do
        pacman -Qi "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
    done
    # base-devel is what provides make/g++: probe the tools, not the group.
    command -v make >/dev/null 2>&1 || missing+=(base-devel)

    if ((${#missing[@]})); then
        # pacman -S resolves from the sync databases; sync the databases
        # (never the packages) only if they have not been fetched yet.
        [ -n "$(ls /var/lib/pacman/sync/*.db 2>/dev/null)" ] \
            || sudo pacman -Sy --noconfirm
        echo "==> Installing missing build dependencies: ${missing[*]}"
        sudo pacman -S --needed --noconfirm "${missing[@]}"
    else
        echo "==> Build dependencies already present; nothing to install."
    fi
fi

echo "==> Configuring…"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr

echo "==> Building…"
cmake --build build --parallel "$(nproc)"

echo "==> Installing…"
sudo cmake --install build

echo "==> Loading the effect into KWin…"
qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.loadEffect wayvibrance \
    || qdbus6 org.kde.kwin.Effects /Effects org.kde.kwin.Effects.loadEffect wayvibrance \
    || true

echo "==> Done. Start the tray with: wayvibrance-tray"
echo "    (or enable 'Start with Plasma' in its tray menu)"
