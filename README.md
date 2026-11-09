[Português](README.pt.md) | **English**

# Way Vibrance

**Digital Vibrance / saturation boost + per-channel RGB gains + per-app
profiles for Linux on Wayland (KDE Plasma)** — the counterpart of Windows'
[vibrance-gui](https://github.com/x64christ/Vibrance-GU) /
[VibranceGUI](https://vibrancegui.com) for platforms where driver tools
don't exist. Works on **any KDE Plasma 6 Wayland session** with OpenGL
compositing: the color grading happens inside KWin itself (the only
compositor that owns the display), as a standard desktop effect — visually
equivalent to driver vibrance.

| Main window | Profile editor | Tray menu |
|---|---|---|
| ![Main window](docs/screenshots/app.png) | ![Profile editor](docs/screenshots/profiles.png) | ![Tray menu](docs/screenshots/tray.png) |

## What you get

- Global vibrance (0–200%) + per-channel R/G/B gains, presets, tray + autostart
- **Per-app/game profiles**: `app:` (app_id/WM_CLASS) or `title:` globs, each
  with its own vibrance + gains, applied **per window** (even unfocused) at
  zero extra GPU cost
- Configurable global hotkey, monitor picker for "only primary monitor"
- CLI, persistent state (`kwinrc`), and a **userspace-only, no-network,
  no-daemon** footprint
- Full guide, D-Bus API and every detail: **[docs/FEATURES.md](docs/FEATURES.md)**

## Requirements

- KDE Plasma 6 (KWin ≥ 6.0), Wayland session, OpenGL compositing.
- Verified in the container matrix: Arch (KWin 6.8), Fedora 44/45, Debian
  13/14 (testing), Ubuntu 26.04/26.10 — same source, no pins.

## Install

**Prebuilt packages**: grab the artifact for your exact distro release from
the [latest release](https://github.com/Machad3x/way-vibrance/releases) —
the effect is compiled against that release's KWin (the effects API has no
cross-version binary promise):

- `.fc44/.fc45` RPM — Fedora; on Atomic (Silverblue/Kinoite):
  `rpm-ostree install ./way-vibrance-*.rpm`
- `.deb` — Debian 13/14-testing (`~debian13`/`~debian14`) and Ubuntu
  26.04/26.10 (`~ubuntuXX.YY`); install with `sudo apt install ./…deb`
- `.pkg.tar.zst` — Arch & derivatives (`sudo pacman -U …`)
- Packages are provided **only for releases still supported upstream**
  (e.g. Ubuntu 25.04/25.10 were dropped at EOL).
- Regenerate everything locally with `scripts/package.sh` (podman matrix)

**Or build (any supported distro):**

```
./scripts/build.sh     # checks deps (installs only what is missing, never
                       # updates the system) → configure → build → install
```

Need manual control: `cmake --build build && sudo cmake --install build`.

Then **log out and back in** so KWin loads the plugin (plugin binaries
cannot be hot-swapped by KWin). Enable via *System Settings → Desktop
Effects → Appearance → Way Vibrance* or just start `wayvibrance-tray`.

## Uninstall

`./scripts/uninstall.sh` — hot-unloads the effect from the running session
and removes exactly what `scripts/build.sh` installed. Manifest-aware
(`build/install_manifest.txt`), re-rootable (`--prefix ~/.local` for
unprivileged installs), previewable (`--dry-run`); sudo is only used when
needed.

Your configuration is **kept** by default and its paths printed — add
`--purge-config` to also strip the `[Effect-wayvibrance]` section and its
profiles sub-group from `kwinrc` and delete `wayvibrancerc` + the autostart
entry.

Installed a **package** instead? Use your package manager:
`pacman -Rns way-vibrance` · `dnf remove way-vibrance` ·
`apt remove way-vibrance` · `rpm-ostree uninstall way-vibrance`.

## Use

- **Tray/window**: sliders for vibrance + RGB channels — each percentage has
  a small circular **↺** button that restores just that slider to its default
  (100%) — presets (0/25/50/75/100/125/150/175/200%), per-app profile editor, hotkey
  field, on/off switches for *Enabled* and *Behaviour* with monitor picker.
  Cards and switches follow the system light/dark colour scheme; the profiles
  pane explains itself when empty; R/G/B sliders carry accessible channel
  colours and presets are a single segmented control. Switches toggle with
  Space or Enter, keep a visible focus ring and still show their position
  when disabled; they have no animation.
- **CLI**: `wayvibrance --help` shows everything
  (`wayvibrance 1.5`, `toggle`, `rgb …`, `primary …`, `app list|set|del|name|reset|focused`).

## Notes that matter

- Cursor is not affected (hardware plane) and RGB gains ≠ 100% switch to the
  offscreen path (slightly heavier, still cheap). The full
  [limitations list](docs/FEATURES.md#known-limitations) and the
  [version-compatibility details](docs/FEATURES.md#version-compatibility)
  live in the docs.
- Everything runs in userspace: no daemons, no network, no telemetry;
  writes only to `~/.config/{kwinrc,wayvibrancerc}` —
  [privacy & footprint](docs/FEATURES.md#privacy--footprint).
- Anti-cheat safety: the grade happens in the desktop compositor, *after*
  the game presents its frames — no game-process interaction at all, the
  same category as driver vibrance settings
  ([details](docs/FEATURES.md#anti-cheat-safety)).

## Roadmap

True-vibrance mode, per-output values, HDR, Vulkan backend, prebuilt
packages/CI, OSD — see **[docs/ROADMAP.md](docs/ROADMAP.md)**.

## Status

Functional and in daily use; current release **v0.6.3** — redesign plus the
completed hardening line: D-Bus trust anchored to the compositor PID and
revalidated on owner changes, string ceilings at every entry point including
window identities, a fully async non-freezable tray, budgeted glob matching
with a versioned fuzz test in CI, bounded shader retries/cache, atomic profile
persistence, and a tamper/symlink-proof uninstaller. The tray now has a
theme-derived card layout, channel-coloured sliders, segmented presets,
per-slider reset buttons and palette-aware switches. The CLI is shipped and
installed as `wayvibrance`; existing configs, per-app profiles and hotkeys
carry over untouched. Multi-distro build support (Arch/Fedora/Debian/Ubuntu,
never updating your system), per-release **prebuilt packages** built and
install-verified in container matrices (attached to releases automatically by
CI), real primary-monitor output picking, and fully bilingual docs remain.

## Development

The versioned matcher test (`tests/glob_match_test.cpp`, runs via
`ctest --test-dir build -R glob_match` and in CI) fuzz-verifies the glob
matcher against the frozen v0.6.1 reference and its step-budget ceilings.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
scripts/dev.sh {load|unload|toggle|status}   # hot-load into the session
scripts/package.sh [fedora|debian|ubuntu|arch]  # prebuilts → dist/
```

Docs (all bilingual — EN default, PT via the switch on each page):
[features & reference](docs/FEATURES.md) ·
[roadmap](docs/ROADMAP.md) ·
[RESEARCH.md](docs/RESEARCH.md) (the KWin API findings behind every design
decision).

## AI assistance

This project was **vibe-coded with AI assistance**: code, research and
documentation were generated and refined together with an AI coding agent
(OpenCode), under the maintainer's direction and review. The full
collaboration history is preserved in the git log.

## License

GPL-2.0-or-later (compatible with KWin). See [LICENSE](LICENSE).
