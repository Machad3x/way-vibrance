#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Capture the demo screenshots into docs/screenshots/ using Spectacle.
# Run it inside a KDE Wayland session with the tray app available.
#
# Usage: ./scripts/capture-screenshots.sh

set -euo pipefail
cd "$(dirname "$0")/.."
OUT="docs/screenshots"
mkdir -p "$OUT"

cli() { wayvibrance "$1" >/dev/null 2>&1 || true; }

# 1) Tray window — open it from the system tray, then confirm.
read -r -p "Open the Way Vibrance tray window, then press Enter… " _
spectacle -a -b -n -o "$OUT/tray.png"

# 2) Before/after: neutral capture, then boosted capture of the full screen.
cli 1.0; sleep 1
spectacle -b -n -o "$OUT/before.png"
cli 1.75; sleep 1
spectacle -b -n -o "$OUT/after.png"
echo "Combine before/after into $OUT/before-after.png (e.g. side by side in an editor)."

# 3) CLI demo: run the commands in a terminal window, capture the active window.
read -r -p "Focus a terminal showing: wayvibrance get / wayvibrance 1.5, then Enter… " _
spectacle -a -b -n -o "$OUT/cli.png"

cli 1.0
ls -la "$OUT"
echo "Done. Now uncomment the Screenshots block in README.md / README.pt.md."
