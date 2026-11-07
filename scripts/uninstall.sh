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

# Configuration paths are needed before any destructive step: kwinrc is
# rewritten by the purge below, so the tool it depends on must exist first.
CFG="${XDG_CONFIG_HOME:-$HOME/.config}"
if [ "$PURGE" = 1 ] && [ -f "$CFG/kwinrc" ] && ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required to purge kwinrc atomically;" >&2
    echo "       install it or remove the [Effect-wayvibrance] groups manually." >&2
    exit 1
fi

# 1. Hot-unload from the running session first: stops the effect's
#    redirects now, and stops KWin re-persisting config we may purge.
QDBUS="$(command -v qdbus6 || command -v qdbus-qt6 || command -v qdbus || true)"
if [ -n "$QDBUS" ]; then
    "$QDBUS" org.kde.KWin /Effects org.kde.kwin.Effects.unloadEffect wayvibrance >/dev/null 2>&1 || true
fi

# 2. Remove installed files. Every candidate — manifest lines re-rooted
#    onto --prefix as well as the canonical file set — goes through ONE
#    validation pipeline: canonicalize first (realpath resolves symlinks,
#    so a planted PREFIX/bin -> /elsewhere cannot smuggle a deletion out
#    of the prefix), then containment under the real prefix, then the
#    project's filename allowlist. A missing or failed manifest simply
#    contributes no candidates; the canonical set alone is still fully
#    validated. Pure bash: no python needed to remove files (only the
#    optional kwinrc purge below uses python3).
REALPREFIX="$(realpath -m -- "$PREFIX" 2>/dev/null || printf '%s' "$PREFIX")"

validate_candidate() {
    local raw="$1" cand rel base
    cand="$(realpath -m -- "$raw" 2>/dev/null)" || cand="$raw"
    case "$cand" in
        "$REALPREFIX"/*) rel="${cand#"$REALPREFIX"/}" ;;
        *) echo "REJECT $cand (resolves outside prefix)" >&2; return 1 ;;
    esac
    base="${cand##*/}"
    case "$rel" in
        bin/wayvibrance|bin/wayvibrance-tray|share/applications/org.kde.wayvibrance-tray.desktop) : ;;
        */plugins/kwin/effects/plugins/wayvibrance.so) : ;;
        *) echo "REJECT $cand (not a Way Vibrance file)" >&2; return 1 ;;
    esac
    printf '%s\n' "$cand"
}

echo "==> Removing installed files (validated candidates under $REALPREFIX)"
CANDIDATES=()
if [ -f build/install_manifest.txt ]; then
    while IFS= read -r l; do
        l="${l%$'\r'}"
        [ -n "$l" ] || continue
        if [[ "$l" =~ /(bin|lib[^/]*|share|include)/(.*)$ ]]; then
            CANDIDATES+=("$REALPREFIX/${BASH_REMATCH[1]}/${BASH_REMATCH[2]}")
        else
            CANDIDATES+=("$l")
        fi
    done < build/install_manifest.txt
else
    echo "    (no build manifest; canonical file set only)"
fi
for libdir in lib lib32 lib64; do
    for sub in plugins/kwin/effects/plugins qt6/plugins/kwin/effects/plugins qt/plugins/kwin/effects/plugins; do
        CANDIDATES+=("$REALPREFIX/$libdir/$sub/wayvibrance.so")
    done
done
CANDIDATES+=("$REALPREFIX/bin/wayvibrance" "$REALPREFIX/bin/wayvibrance-tray"
             "$REALPREFIX/share/applications/org.kde.wayvibrance-tray.desktop")

MANIFEST=()
REJECTED=0
for cand in "${CANDIDATES[@]}"; do
    if out="$(validate_candidate "$cand")"; then
        [ -n "$out" ] && MANIFEST+=("$out")
    else
        REJECTED=$((REJECTED + 1))
    fi
done
if [ "${#MANIFEST[@]}" -gt 0 ]; then
    mapfile -t MANIFEST < <(printf '%s\n' "${MANIFEST[@]}" | sort -u)
fi
echo "    validated ${#MANIFEST[@]} path(s), rejected $REJECTED"

if [ "${#MANIFEST[@]}" = 0 ]; then
    echo "==> Nothing validated under $REALPREFIX; nothing to remove"
elif [ "$DRY" = 1 ]; then
    printf 'DRY: rm -f %s\n' "${MANIFEST[@]}"
else
    if needs_sudo "${MANIFEST[@]}" && command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    fi
    printf '%s\0' "${MANIFEST[@]}" | $SUDO xargs -0 rm -f
fi

# 3. Configuration: purged only on demand; always reported.
if [ "$PURGE" = 1 ]; then
    echo "==> Purging configuration"
    if [ -f "$CFG/kwinrc" ]; then
        if [ "$DRY" = 1 ]; then
            echo "DRY: python3 strip [Effect-wayvibrance]* groups from $CFG/kwinrc"
        else
            python3 - "$CFG/kwinrc" <<'PY'
import os
import stat
import sys
import tempfile

path = os.path.realpath(sys.argv[1])
with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
    lines = fh.readlines()

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

# Atomic replace: a crash, full disk or signal must never leave kwinrc
# truncated (KConfig would then lose every unrelated group too).
st = os.stat(path)
fd, tmp = tempfile.mkstemp(prefix=".kwinrc.", dir=os.path.dirname(path))
try:
    with os.fdopen(fd, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.writelines(out)
    os.chmod(tmp, stat.S_IMODE(st.st_mode))
    os.replace(tmp, path)
except BaseException:
    try:
        os.unlink(tmp)
    except OSError:
        pass
    raise
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
