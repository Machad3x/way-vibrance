#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Uninstall a manually built/installed Way Vibrance (the file copies done
# by scripts/build.sh). If you installed a PACKAGE, use your package
# manager instead: pacman -Rns way-vibrance · dnf remove way-vibrance ·
# apt remove way-vibrance · rpm-ostree uninstall way-vibrance.
#
# Usage: scripts/uninstall.sh [--prefix PATH] [--purge-config] [--dry-run]
#   --prefix PATH     install prefix to clean (default /usr; no sudo is
#                     used for prefixes inside the user's home)
#   --purge-config    also delete the app's configuration: the
#                     [Effect-wayvibrance] section (and the profiles child
#                     group) in kwinrc, wayvibrancerc, and the autostart
#                     entry. Without it, configs are KEPT and their paths
#                     are printed so you can review/remove them yourself.
#   --dry-run         print what would be removed; change nothing.

set -euo pipefail
cd "$(dirname "$0")/.."

PREFIX="/usr"
PURGE=0
DRY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)       PREFIX="$2"; shift 2 ;;
        --purge-config) PURGE=1; shift ;;
        --dry-run)      DRY=1; shift ;;
        *) echo "unknown option: $1 (see the header of this script)" >&2; exit 1 ;;
    esac
done

run() {
    if [ "$DRY" = 1 ]; then
        echo "DRY: $*"
    else
        "$@"
    fi
}

# sudo is decided lazily from what we actually delete: files that live in
# directories this user cannot write (e.g. /usr) need it, everything else
# (a --prefix inside the home, nonexistent leftovers from multi-prefix
# manifests, dry runs) never does.
SUDO=""
needs_sudo() {
    local f d
    for f in "$@"; do
        [ -e "$f" ] || continue
        d="$(dirname "$f")"
        while [ ! -e "$d" ]; do d="$(dirname "$d")"; done
        [ -w "$d" ] || return 0
    done
    return 1
}

# 1. Hot-unload from the running session first: stops the effect's
#    redirects now, and stops KWin re-persisting config we may purge.
QDBUS="$(command -v qdbus6 || command -v qdbus-qt6 || command -v qdbus || true)"
if [ -n "$QDBUS" ]; then
    "$QDBUS" org.kde.KWin /Effects org.kde.kwin.Effects.unloadEffect wayvibrance >/dev/null 2>&1 || true
fi

# 2. Remove installed files. Prefer the exact manifest written by the
#    install step; its recorded prefix is detected and re-rooted onto
#    --prefix (so `--prefix ~/.local` also works when the tree was built
#    for /usr, and vice versa without privileges). Fall back to the
#    canonical file set when there is no manifest.
if [ -f build/install_manifest.txt ]; then
    echo "==> Removing installed files (manifest re-rooted at $PREFIX + canonical set)"
    mapfile -t MANIFEST < <(
        python3 - "$PREFIX" build/install_manifest.txt <<'PY'
import sys, re
prefix, path = sys.argv[1], sys.argv[2]
for line in open(path):
    l = line.strip()
    if not l:
        continue
    m = re.match(r'^(.*?)/(bin|lib[^/]*|share|include)/', l)
    if m:
        print(prefix + "/" + l[m.start(2):])
    else:
        print(l)
PY
        { find "$PREFIX"/lib*/plugins/kwin/effects/plugins "$PREFIX"/lib*/qt6/plugins/kwin/effects/plugins \
              -maxdepth 1 -name wayvibrance.so 2>/dev/null || true
          echo "$PREFIX/bin/wayvibrance"
          echo "$PREFIX/bin/wayvibrance-tray"
          echo "$PREFIX/share/applications/org.kde.wayvibrance-tray.desktop"; }
    )
    mapfile -t MANIFEST < <(printf '%s\n' "${MANIFEST[@]}" | sort -u)
    if [ "$DRY" = 1 ]; then
        printf 'DRY: rm -f %s\n' "${MANIFEST[@]}"
    else
        if needs_sudo "${MANIFEST[@]}" && command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        fi
        printf '%s\0' "${MANIFEST[@]}" | $SUDO xargs -0 rm -f
    fi
else
    echo "==> No build manifest found; removing the canonical file set under $PREFIX"
    mapfile -t FILES < <(
        { find "$PREFIX"/lib*/qt6/plugins/kwin/effects/plugins -maxdepth 1 -name wayvibrance.so 2>/dev/null || true
          echo "$PREFIX/bin/wayvibrance"
          echo "$PREFIX/bin/wayvibrance-tray"
          echo "$PREFIX/share/applications/org.kde.wayvibrance-tray.desktop"; } | sort -u
    )
    if needs_sudo "${FILES[@]}" && command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    fi
    for f in "${FILES[@]}"; do
        [ -e "$f" ] || continue
        run $SUDO rm -f "$f"
        echo "    removed $f"
    done
fi

# 3. Configuration: purged only on demand; always reported.
CFG="${XDG_CONFIG_HOME:-$HOME/.config}"
if [ "$PURGE" = 1 ]; then
    echo "==> Purging configuration"
    if [ -f "$CFG/kwinrc" ]; then
        if [ "$DRY" = 1 ]; then
            echo "DRY: python3 strip [Effect-wayvibrance]* groups from $CFG/kwinrc"
        else
            python3 - "$CFG/kwinrc" <<'PY'
import sys
path = sys.argv[1]
lines = open(path).read().splitlines(keepends=True)
drop = {"[Effect-wayvibrance]", "[Effect-wayvibrance][WayVibranceAppRules]"}
out, skipping = [], False
for ln in lines:
    stripped = ln.rstrip("\r\n")
    if stripped in drop:
        skipping = True
        continue
    if skipping and ln.startswith("["):
        skipping = False
    if not skipping:
        out.append(ln)
open(path, "w").writelines(out)
print("    kwinrc: effect section + profiles removed")
PY
        fi
    else
        echo "    (no $CFG/kwinrc)"
    fi
    if [ -e "$CFG/wayvibrancerc" ]; then
        run rm -f "$CFG/wayvibrancerc"
        [ "$DRY" = 1 ] || echo "    removed $CFG/wayvibrancerc"
    fi
    if [ -e "$CFG/autostart/org.kde.wayvibrance-tray.desktop" ]; then
        run rm -f "$CFG/autostart/org.kde.wayvibrance-tray.desktop"
        [ "$DRY" = 1 ] || echo "    removed $CFG/autostart/org.kde.wayvibrance-tray.desktop"
    fi
else
    cat <<INFO
==> Configuration kept (re-run with --purge-config to remove):
    $CFG/kwinrc          → group [Effect-wayvibrance] + [Effect-wayvibrance][WayVibranceAppRules]
    $CFG/wayvibrancerc   → global hotkey and behaviour settings
    $CFG/autostart/org.kde.wayvibrance-tray.desktop (if 'Start with Plasma' was on)
INFO
fi

echo
echo "==> Done. If the effect had been running this session, a fresh login"
echo "    guarantees a pristine compositor state (it was unloaded live too)."
