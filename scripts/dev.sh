#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Reload the *installed* wayvibrance plugin in the running KWin.
# NOTE: KWin cannot dlclose() effect plugins safely; after a rebuild you
# usually need to log out and back in (or restart the session) for the new
# binary to take effect. Use this to unload/load around quick experiments.

set -euo pipefail

case "${1:-}" in
    load)
        qdbus6 org.kde.kwin.Effects /Effects org.kde.kwin.Effects.loadEffect wayvibrance
        ;;
    unload)
        qdbus6 org.kde.kwin.Effects /Effects org.kde.kwin.Effects.unloadEffect wayvibrance
        ;;
    toggle)
        qdbus6 org.kde.kwin.Effects /Effects org.kde.kwin.Effects.toggleEffect wayvibrance
        ;;
    status)
        qdbus6 org.kde.kwin.Effects /Effects org.kde.kwin.Effects.isEffectLoaded wayvibrance
        ;;
    *)
        echo "usage: $0 {load|unload|toggle|status}" >&2
        exit 1
        ;;
esac
