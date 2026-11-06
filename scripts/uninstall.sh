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

# 2. Remove installed files: the manifest written by the install step
#    (re-rooted onto --prefix) plus the canonical file set — but every
#    candidate must pass the allowlist below (absolute, canonical, inside
#    the prefix, and one of Way Vibrance's own files) before anything is
#    deleted. A tampered or stale workspace manifest can therefore never
#    turn sudo rm into an arbitrary deletion tool.
ALLOWLIST_OK=1
if [ -f build/install_manifest.txt ]; then
    echo "==> Removing installed files (manifest re-rooted at $PREFIX + canonical set)"
    mapfile -t MANIFEST < <(
        python3 - "$PREFIX" build/install_manifest.txt <<'PY'
import os, re, sys

prefix = os.path.realpath(sys.argv[1])
seen, rejected = set(), 0
roots = [l.split("\x00", 1)[0].strip() for l in open(sys.argv[2]) if l.strip()]
# Canonical set: always attempted, covers manifest-less and drifted trees.
roots.append("$CANONICAL$")

for path in roots:
    if path == "$CANONICAL$":
        for libdir in ("lib", "lib32", "lib64"):
            for root in (prefix + "/" + libdir,):
                for sub in ("plugins/kwin/effects/plugins",
                            "qt6/plugins/kwin/effects/plugins",
                            "qt/plugins/kwin/effects/plugins"):
                    cand = root + "/" + sub + "/wayvibrance.so"
                    if cand not in seen:
                        seen.add(cand)
                        print(cand)
        for cand in (prefix + "/bin/wayvibrance", prefix + "/bin/wayvibrance-tray",
                     prefix + "/share/applications/org.kde.wayvibrance-tray.desktop"):
            if cand not in seen:
                seen.add(cand)
                print(cand)
        continue
    m = re.match(r"^(.*?)/(bin|lib[^/]*|share|include)/", path)
    cand = prefix + "/" + path[m.start(2):] if m else path
    cand = os.path.realpath(cand)
    rel = os.path.relpath(cand, prefix)
    base = os.path.basename(cand)
    owner = (rel in ("bin/wayvibrance", "bin/wayvibrance-tray",
                     "share/applications/org.kde.wayvibrance-tray.desktop")
             or (base == "wayvibrance.so" and "/plugins/kwin/effects/plugins/" in cand))
    if not owner:
        rejected += 1
        print("REJECT %s" % cand, file=sys.stderr)
        continue
    if rel in (os.curdir, os.pardir) or rel.startswith(os.pardir + os.sep):
        rejected += 1
        print("REJECT %s (outside prefix)" % cand, file=sys.stderr)
        continue
    if cand not in seen:
        seen.add(cand)
        print(cand)

print("validated %d path(s), rejected %d" % (len(seen), rejected), file=sys.stderr)
PY
    ) || ALLOWLIST_OK=0
fi
if [ "${ALLOWLIST_OK}" = 0 ] || [ -z "${MANIFEST[*]:-}" ]; then
    if [ -f build/install_manifest.txt ]; then
        echo "==> Manifest failed validation; removing the canonical file set under $PREFIX only"
    else
        echo "==> No build manifest found; removing the canonical file set under $PREFIX"
    fi
    ALLOWLIST_OK=1
    MANIFEST=()
    for f in "$PREFIX"/lib*/plugins/kwin/effects/plugins/wayvibrance.so \
             "$PREFIX"/lib*/qt6/plugins/kwin/effects/plugins/wayvibrance.so \
             "$PREFIX"/bin/wayvibrance "$PREFIX"/bin/wayvibrance-tray \
             "$PREFIX"/share/applications/org.kde.wayvibrance-tray.desktop; do
        MANIFEST+=("$f")
    done
fi
mapfile -t MANIFEST < <(printf '%s\n' "${MANIFEST[@]}" | sort -u)
if [ "$DRY" = 1 ]; then
    printf 'DRY: rm -f %s\n' "${MANIFEST[@]}"
else
    if needs_sudo "${MANIFEST[@]}" && command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    fi
    printf '%s\0' "${MANIFEST[@]}" | $SUDO xargs -0 rm -f
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
