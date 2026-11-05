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
- Verified: Arch (KWin 6.8), Fedora, Debian 13 and Ubuntu 25.04 (6.3.4) —
  the same source compiles across 6.0 → latest, no version pins.

## Install

**Prebuilt packages** (next release onwards): grab the artifact for your
distro from the [latest release](https://github.com/Machad3x/way-vibrance/releases)
— `.rpm` (Fedora; on Silverblue/Kinoite: `rpm-ostree install ./way-vibrance-*.rpm`),
`.deb` (Debian 13+/Ubuntu 24.10+/Neon), `.pkg.tar.zst` (Arch).

**Or build (any supported distro):**

```
./scripts/build.sh          # installs only missing dev packages — never updates your system
sudo cmake --install build  # (build.sh does this automatically when run with sudo)
```

Then **log out and back in** so KWin loads the plugin (plugin binaries
cannot be hot-swapped by KWin). Enable via *System Settings → Desktop
Effects → Appearance → Way Vibrance* or just start `wayvibrance-tray`.

## Use

- **Tray/window**: sliders for vibrance + RGB channels, presets
  (50/100/125/150/200%), per-app profile editor, hotkey field, behaviour
  checkboxes with monitor picker.
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

## Roadmap

True-vibrance mode, per-output values, HDR, Vulkan backend, prebuilt
packages/CI, OSD — see **[docs/ROADMAP.md](docs/ROADMAP.md)**.

## Status

Functional and in daily use; current release **v0.5.0** (primary-monitor
picker). Per-app profiles shipped in v0.4.0; multi-distro build support,
packaging and docs restructure are queued for the next release.

## Development

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
scripts/dev.sh {load|unload|toggle|status}   # hot-load into the session
scripts/package.sh [fedora|debian|ubuntu|arch]  # prebuilts → dist/
```

Docs: [features & reference](docs/FEATURES.md) ·
[roadmap](docs/ROADMAP.md) ·
[RESEARCH.md](docs/RESEARCH.md) (Portuguese — the KWin API findings behind
every design decision).

## AI assistance

This project was **vibe-coded with AI assistance**: code, research and
documentation were generated and refined together with an AI coding agent
(OpenCode), under the maintainer's direction and review. The full
collaboration history is preserved in the git log.

## License

GPL-2.0-or-later (compatible with KWin). See [LICENSE](LICENSE).
