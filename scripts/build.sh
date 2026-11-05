#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Install build dependencies (Arch Linux / CachyOS), build and install
# Way Vibrance, then load the effect into the running KWin session.

set -euo pipefail
cd "$(dirname "$0")/.."

if command -v pacman >/dev/null 2>&1; then
    echo "==> Installing build dependencies (pacman)…"
    sudo pacman -Syu --needed --noconfirm \
        base-devel cmake extra-cmake-modules kwin \
        qt6-base qt6-tools kconfig kcoreaddons kdbusaddons kglobalaccel \
        vulkan-headers libdrm wayland
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
