#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Publish way-vibrance to GitHub: create the repo (main as default
# branch), push, tag and create the release. Works with either:
#   a) an authenticated `gh` CLI  (gh auth login), or
#   b) a GITHUB_TOKEN env var (classic PAT with `repo`, or fine-grained
#      with Administration:write + Contents:write on the account).
#
# Usage: ./scripts/publish.sh [repo-name] [public|private]

set -euo pipefail
cd "$(dirname "$0")/.."

REPO_NAME="${1:-way-vibrance}"
VISIBILITY="${2:-public}"
VERSION="$(grep -m1 'project(way-vibrance VERSION' CMakeLists.txt | awk '{print $3}')"
TAG="v${VERSION}"
DESCRIPTION="Digital Vibrance + per-channel RGB gains + per-app profiles for KDE Plasma on Wayland (vibrance-gui counterpart, AI-assisted)"

git branch -M main

if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
    echo "==> Using gh CLI"
    gh auth setup-git || true
    OWNER="$(gh api user -q .login)"
    gh repo view "$OWNER/$REPO_NAME" >/dev/null 2>&1 \
        || gh repo create "$REPO_NAME" "--$VISIBILITY" --description "$DESCRIPTION"
    REMOTE="https://github.com/$OWNER/$REPO_NAME.git"
    if git remote get-url origin >/dev/null 2>&1; then
        git remote set-url origin "$REMOTE"
    else
        git remote add origin "$REMOTE"
    fi
    git tag -a "$TAG" -m "Way Vibrance $TAG" 2>/dev/null || true
    git push -u origin main --follow-tags
    gh release view "$TAG" >/dev/null 2>&1 || gh release create "$TAG" \
        --title "Way Vibrance $TAG" --notes "$(cat <<EOF
Release ${VERSION}.

**Features**
- Global saturation/vibrance boost through KWin's built-in linear shader (zero cost when neutral)
- Per-channel R/G/B gains via a conditional offscreen GLSL pass
- **Per-app/game profiles**: \`app:\` (Wayland app_id / X11 WM_CLASS) or \`title:\` globs, each with its own vibrance + channel gains and an optional cosmetic display name — applied per window, even unfocused, at no extra GPU cost
- Tray app: two-pane UI, per-profile editor window with Apply + presets (50/100/125/150/200%), behaviour settings, monitor picker for "only primary monitor" (a fixed output — not the pointer)
- Configurable global hotkey (KGlobalAccel, conflict detection, persisted)
- CLI: \`wayvibrance 1.25 | toggle | get | rgb | primary [on|off|get|outputs|output <name>] | app [list|set|del|name|reset|focused]\`
- Prebuilt packages attached for currently supported releases (Fedora RPM incl. rpm-ostree/Atomic, Debian/Ubuntu DEB, Arch pkg), each built and install-verified in a container of its own release

**Compatibility** — any KDE Plasma 6 Wayland session (source builds against KWin 6.0–6.9+). Requires OpenGL compositing. After upgrading, log out and back in so KWin loads the new effect binary.

*This project was vibe-coded with AI assistance — see README.*
EOF
)"
elif [ -n "${GITHUB_TOKEN:-}" ]; then
    echo "==> Using GITHUB_TOKEN via API"
    API="https://api.github.com"
    AUTH=(-H "Authorization: Bearer $GITHUB_TOKEN" -H "Accept: application/vnd.github+json")
    OWNER="$(curl -fsS "$API/user" "${AUTH[@]}" | sed -n 's/.*"login":[[:space:]]*"\([^"]*\)".*/\1/p')"
    code=$(curl -fsS -o /dev/null -w "%{http_code}" "$API/repos/$OWNER/$REPO_NAME" "${AUTH[@]}" || echo 000)
    if [ "$code" != "200" ]; then
        priv=$([ "$VISIBILITY" = "private" ] && echo true || echo false)
        curl -fsS "$API/user/repos" "${AUTH[@]}" \
            -d "{\"name\":\"$REPO_NAME\",\"description\":\"$DESCRIPTION\",\"private\":$priv,\"has_issues\":true}" >/dev/null
    fi
    REMOTE="https://x-access-token:$GITHUB_TOKEN@github.com/$OWNER/$REPO_NAME.git"
    git remote remove origin 2>/dev/null || true
    git remote add origin "$REMOTE"
    git tag -a "$TAG" -m "Way Vibrance $TAG" 2>/dev/null || true
    git push -u origin main --follow-tags
    json=$(python3 - "$TAG" "$VERSION" <<'PY'
import json, sys
tag, ver = sys.argv[1], sys.argv[2]
notes = (f"Release {ver}.\n\n"
         "- Global vibrance boost via KWin's built-in linear saturation uniform\n"
         "- Per-channel R/G/B gains through a conditional offscreen GLSL pass\n"
         "- Per-app/game profiles: app:/title: globs with per-profile vibrance, "
         "channel gains and cosmetic display names (per-window, even unfocused)\n"
         "- Tray: two-pane UI, profile editor with Apply/presets, monitor picker "
         "for primary-monitor mode (fixed output, not the pointer)\n"
         "- Configurable global hotkey (KGlobalAccel) with conflict detection\n"
         "- CLI: wayvibrance get/set/toggle/rgb/primary/app\n"
         "- Prebuilt packages attached for currently supported releases "
         "(Fedora/Debian/Ubuntu/Arch), install-verified per release\n\n"
         f"Works on any KDE Plasma 6 Wayland session (KWin >= 6.0, OpenGL). "
         "After upgrading, log out and back in so KWin reloads the effect. "
         "Vibe-coded with AI assistance — see README.")
print(json.dumps({"tag_name": tag, "name": f"Way Vibrance {ver}", "body": notes, "draft": False, "prerelease": False}))
PY
)
    curl -fsS "$API/repos/$OWNER/$REPO_NAME/releases" "${AUTH[@]}" -d "$json" >/dev/null
    git remote set-url origin "https://github.com/$OWNER/$REPO_NAME.git"
else
    echo "No credentials found. Do one of:" >&2
    echo "  a) sudo pacman -S github-cli && gh auth login -h github.com -p https -s repo,read:org -w" >&2
    echo "  b) GITHUB_TOKEN=<PAT> ./scripts/publish.sh" >&2
    exit 1
fi

OWNER="${OWNER:-$(git remote get-url origin | sed -E 's#.*github.com[:/]([^/]+)/.*#\1#')}"
echo
echo "==> Done: https://github.com/$OWNER/$REPO_NAME  (release $TAG)"
