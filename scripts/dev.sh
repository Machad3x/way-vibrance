#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Reload the *installed* wayvibrance plugin in the running KWin.
# NOTE: KWin cannot dlclose() effect plugins safely; after a rebuild you
# usually need to log out and back in (or restart the session) for the new
# binary to take effect. Use this to unload/load around quick experiments.

set -euo pipefail

kwin_effects() {
    # KWin 6.8 hosts /Effects under org.kde.KWin; fall back to the legacy name.
    local qdbus_bin
    qdbus_bin="$(command -v qdbus6 || command -v qdbus-qt6 || command -v qdbus)"
    if out=$("$qdbus_bin" org.kde.KWin /Effects "org.kde.kwin.Effects.$1" "${@:2}" 2>/dev/null); then
        echo "$out"
    else
        "$qdbus_bin" org.kde.kwin.Effects /Effects "org.kde.kwin.Effects.$1" "${@:2}"
    fi
}

case "${1:-}" in
    load)
        kwin_effects loadEffect wayvibrance
        ;;
    unload)
        kwin_effects unloadEffect wayvibrance
        ;;
    toggle)
        kwin_effects toggleEffect wayvibrance
        ;;
    status)
        kwin_effects isEffectLoaded wayvibrance
        ;;
    *)
        echo "usage: $0 {load|unload|toggle|status}" >&2
        exit 1
        ;;
esac
