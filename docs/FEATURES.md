[English](FEATURES.md) | [Português](FEATURES.pt.md)

# Way Vibrance — Features & Reference

The full guide; the [README](../README.md) keeps only what you need to build
and run. Back to [README](../README.md) · [Roadmap](ROADMAP.md).

## Table of contents
- [How it works](#how-it-works)
- [Per-app profiles (the deep dive)](#per-app-profiles)
- [Hotkey & behaviour settings](#hotkey--behaviour-settings)
- [CLI reference](#cli-reference)
- [D-Bus API](#d-bus-api)
- [Configuration files](#configuration-files)
- [Version compatibility](#version-compatibility)
- [Parity with vibrance-gui](#parity-with-vibrance-gui)
- [Known limitations](#known-limitations)
- [Privacy & footprint](#privacy--footprint)
- [Anti-cheat safety](#anti-cheat-safety)
- [Screenshots](#screenshots)

## How it works

- **`effect/`** — KWin 6 desktop-effect plugin (C++), with cost-tiered
  rendering paths:
  - **Vibrance**: `WindowPaintData::multiplySaturation()` at the end of the
    effect chain (`chainPosition 99`). KWin's scene renderer applies it with
    its built-in shader, **in linear color space**, without grabbing windows
    or adding passes.
  - **Per-channel RGB gains**: when any channel differs from 100%, windows
    are additionally routed through `OffscreenEffect` with a custom GLSL
    shader multiplying the channels in linear space. With all channels at
    100% nothing is redirected and the cost is back to zero.
  - **All GL work happens inside `drawWindow`** (the only place the GL
    context is guaranteed current): shader instances are created and
    `rgbGain` uniforms uploaded there — events from D-Bus/config only mark
    state dirty and request a repaint. Each distinct gain set gets its own
    `GLShader` instance; only windows that actually use gains are redirected.
- **`tray/`** — `wayvibrance-tray`: Qt 6 tray app (throttled live sliders,
  presets, per-app profile editor, global hotkey, autostart, behaviour
  settings, monitor picker).
- **`cli/`** — `wayvibrance`: terminal control over the same D-Bus interface
  (`wayvibrance --help` for everything).
- Neutral state (`1.0` everywhere, no profiles) ⇒ `isActive() == false` ⇒
  the effect is out of the rendering chain: **zero cost**.

## Per-app profiles

The counterpart of vibrance-gui's per-game profiles — and one step further:
they apply **per window**, not per focus, so a background stream or a second
monitor's game keeps its look.

- **Pattern** (case-insensitive glob; `*` and `?` wildcards):
  - `app:` (default) matches the **Wayland app_id** / **X11 WM_CLASS** —
    e.g. `firefox`, `org.kde.dolphin`, `*steam*`.
  - `title:` matches the **window caption** — e.g. `title:*Elden Ring*`
    (handy when the app_id is generic, like Proton launchers).
- **Values**: vibrance (0–200%) and optional per-channel R/G/B gains
  (0–200%) — matching windows override the global values.
- **Display name**: purely cosmetic label in the list (`org.kde.dolphin`
  shown as "Dolphin" automatically; your own string overrides). Matching
  always uses the pattern.
- **Tray**: state pulls are async with a bounded timeout — a busy or
  wedged compositor cannot freeze the tray UI; list with friendly names
  and `[+RGB …]` badge; *From last
  focused…* pre-fills the app id of the last-used window (two clicks to
  enroll a game); double-click opens the editor window (sliders + presets
  50/100/125/150/200% + Reset RGB); **Apply** commits live, **OK** closes
  (confirming if something is unapplied; a never-applied new profile is
  saved silently on OK); the dialog hides features the running effect build
  cannot do.
- **Persistence**: `[Effect-wayvibrance][WayVibranceAppRules]` in `kwinrc`.
- **Limits** (local-DoS containment): at most 64 profiles, pattern up to
  128 characters, display name up to 64; the effect, the tray dialog and the
  config load all enforce the same ceilings, rejecting non-finite values
  rather than letting them reach the compositor. Patterns are matched by a
  dedicated linear-time glob matcher (no regex engine, so no backtracking
  cost to reason about) with behavior verified identical to the previous
  regex translation.
  Per-profile gain combinations are shader-cached in a bounded pool (72
  compiled variants): exact gains compile on demand, and once full the
  cache first recycles variants no window is using — falling back to the
  nearest cached variant (<1% visual difference) if all are on screen —
  so endless gain-editing cannot grow GPU memory or compiler work.

## Hotkey & behaviour settings

- **Global hotkey**: unbound by default (we don't fight Plasma's shortcuts).
  Click the *Global hotkey* field and press a combination — applied and
  persisted immediately (in `wayvibrancerc`, re-registered with KGlobalAccel
  each session), with conflict detection and a "Clear" button. Also editable
  in *System Settings → Shortcuts → Applications → Way Vibrance* (changes
  there are mirrored back). Requires the tray running — enable "Start with
  Plasma".
- **Behaviour** (one check each): *Open directly to the tray on startup*,
  *Start with Plasma* (autostart, shown with the proper icon in Plasma's
  Autostart settings), *Only affect the primary monitor* — grading applies
  only to the **chosen output**: a *Primary monitor* dropdown appears under
  the checkbox (picks by connector name, e.g. `DP-1`; "Automatic" = KWin's
  first output; the picked output's list refreshes on hotplug).

## CLI reference

Full and authoritative output: `wayvibrance --help`. Quick map:

```
wayvibrance                     # current vibrance
wayvibrance 1.25                # set vibrance (also: on/off/toggle/load)
wayvibrance rgb 1.2 1 0.9       # channel gains (rgb reset, getrgb)
wayvibrance primary on|off|get|outputs|output <name>|output-get
wayvibrance app list|set|del|name|reset|focused
```

## D-Bus API

Service `org.wayvibrance`, object `/Vibrance`, interface `org.wayvibrance`.

| Methods | Signals |
|---|---|
| `vibrance` / `setVibrance(v)` | `vibranceChanged(v)` |
| `enabled` / `setEnabled(b)` / `toggleEnabled` | `enabledChanged(b)` |
| `redGain`/`greenGain`/`blueGain` / `setRGBGains(r,g,b)` | `rgbGainsChanged(r,g,b)` |
| `primaryOnly` / `setPrimaryOnly(b)` | `primaryOnlyChanged(b)` |
| `primaryOutputName` / `setPrimaryOutputName(name)` / `screenList()` | `primaryOutputNameChanged(name)` |
| `appRules` / `setAppRule(pat,v)` / `setAppRuleFull(pat,v,r,g,b)` / `setAppRuleName(pat,name)` / `removeAppRule(pat)` / `clearAppRules` / `lastFocusedAppId()` | `appRulesChanged(rules)` |

Profile serialization: `kind:pattern|vibrance[|r,g,b[|display-name]]`
(`kind` ∈ `app`,`title`; patterns and names never contain `|`).

## Configuration files

| File | Contents |
|---|---|
| `~/.config/kwinrc` | `[Effect-wayvibrance]`: `Vibrance`, `Enabled`, `PrimaryOnly`, `PrimaryOutput`, `RedGain/GainGreenGain/BlueGain`; child group `[WayVibranceAppRules]` with one percent-encoded entry per profile |
| `~/.config/wayvibrancerc` | hotkey (`[Shortcuts] GlobalToggleShortcut`) and `[General] StartHidden` |
| `~/.config/autostart/org.kde.wayvibrance-tray.desktop` | autostart toggle |

`scripts/uninstall.sh [--prefix PATH] [--purge-config] [--dry-run]` removes
a manually installed build, hot-unloading the effect first; every removal
candidate (build manifest included, symlink-resolved and re-rooted paths
first canonicalized) is validated against the real prefix and a file
allowlist before anything is deleted, so neither a tampered workspace
manifest nor a planted `bin ->` symlink can smuggle unrelated paths into
`sudo rm`. Package installs are uninstalled with the package manager.

## Version compatibility

- The tray and the CLI talk **pure D-Bus** (`org.wayvibrance` plus
  `org.kde.kwin.Effects`), stable across Plasma 6 — they depend on no
  specific KDE version.
- The effect plugin is compiled **against the KWin installed on your
  machine** (like every third-party KWin effect: the effects API has no
  binary compatibility guarantee between releases). The same *source*
  compiles on KWin **6.0 → 6.9+**, adapting to `drawWindow()` signature
  changes (`QRegion`→`Region` in 6.6, `void`→`bool` in 6.8) via `decltype`
  on the base signature, and to the output-class reshuffle (Screen/Output/
  LogicalOutput) via `auto`-deduced iteration + `__has_include` — no
  version pins. After a large Plasma update, rerun `scripts/build.sh`.
- Verified container builds: Arch, Fedora 44/45, Debian 13/14 (testing) and
  Ubuntu 26.04/26.10 — each artifact additionally *installed and smoke-verified*
  in a fresh container of its release — plus the daily driver at KWin 6.8.2.
- **Prebuilt policy**: one binary per distro release, each compiled in a
  throwaway container of that release (`scripts/package.sh`) — mixing a
  plugin built against one KWin minor with another can crash on load, since
  the effects API has no ABI stability promise. Artifacts embed their
  target: `.fc44/.fc45`, `~debian13/~debian14`, `~ubuntu26.04…`.
  Releases that reach upstream EOL are dropped from the matrix (Ubuntu
  25.04/25.10 were removed once Canonical ended support).

## Parity with vibrance-gui

| Feature | vibrance-gui (Windows) | Way Vibrance | status |
|---|---|---|---|
| Global vibrance boost | driver CTM | KWin shader uniform | ✅ |
| Per-channel R/G/B gains | driver Color page | offscreen GLSL pass | ✅ |
| Live adjustment | driver API | D-Bus `org.wayvibrance` | ✅ |
| Persistence | registry | `kwinrc` | ✅ |
| Tray + autostart | ✅ | ✅ | ✅ |
| Presets | profiles | 50/100/125/150/200% | ✅ |
| Toggle hotkey | ✅ | KGlobalAccel, key configurable | ✅ |
| Apply only to one monitor | — (profile-based) | "primary monitor" mode (picked output) | ✅ |
| Auto per-app/game profiles | ✅ (window focus) | per-window rules (`app:`/`title:` globs) — applies even unfocused | ✅ |
| Distinct value per monitor | ✅ | — (restriction exists since v0.5.0; independent values per output need a per-output uniform/scene pass) | 🔜 |
| Sound/OSD on profile switch | ✅ | layer-shell OSD | 🔜 |

## Known limitations

- **The mouse cursor is not affected** (it is a separate hardware plane in
  KMS) — the same trade-off as KWin's own colorblindness-correction effect.
- **RGB gains ≠ 100% enable the offscreen path** (per-window grab + shader):
  slightly heavier than pure vibrance, which stays on the zero-cost native
  uniform. Keep the channels at 100% for the light mode.
- **OpenGL compositing only** (Vulkan on the roadmap; under Vulkan the
  effect deliberately does not load rather than silently no-op).
- The plugin binary is matched to the installed KWin; a quick
  `scripts/build.sh` (or the matching prebuilt package) after major updates
  keeps it in step.
- "Primary monitor" mode targets the output you pick by connector name —
  with no pick, the **first** output is used; re-pick after changing the
  monitor setup.
- With any window effect active, exclusive-fullscreen windows may lose the
  direct-scanout fast path in the compositor (small latency cost; worth
  measuring).
- Immutable distros (Silverblue/Kinoite/SteamOS): KWin only loads effects
  from system plugin directories — user-scope paths were tested and do not
  work (6.3–6.8). Use the prebuilt RPM with `rpm-ostree install`. On
  SteamOS specifically, the gamescope gaming session runs no KWin, so the
  effect applies only to the desktop session.

## Privacy & footprint

Everything runs in **userspace**:
- no daemons, no kernel modules, no network access, no telemetry;
- D-Bus on the **session bus only** (`org.wayvibrance` service + the tray's
  own component name); no system bus usage;
- the tray trusts `org.wayvibrance` only when the connection owning the
  name is the **compositor process itself** — the PID is cross-checked
  against `org.kde.KWin` on attach and re-verified on every ownership
  change, and state arriving via signals as well as method replies passes
  the same finite/range/list validation — so a peer that grabs the name
  first (or swaps owners afterwards) cannot spoof the UI or feed it
  hostile values;
- writes only to `~/.config/kwinrc` (effect state, managed by KWin itself),
  `~/.config/wayvibrancerc` (hotkey/behaviour) and the optional autostart
  `.desktop`;
- `scripts/build.sh` installs only missing packages and **never updates
  the system**; inside containers it refuses to write to `/usr` by default;
- system-wide installation (`sudo cmake --install`) is only the file copy
  step — the software never needs root at runtime.

## Anti-cheat safety

**Can using Way Vibrance get me banned in online games?** By architecture,
there is no plausible vector — and it is worth understanding why, because
the answer comes from *where* the effect lives, not from promises.

- Everything runs **inside KWin, the desktop compositor**, as
  post-processing of the final composited frame — i.e. **after** a game has
  already rendered and presented its output. The game's process, memory and
  rendering APIs are never touched; the game cannot even detect it.
- This is the same mechanism as KDE's own shipped colorblindness-correction
  effect, using the identical public effect API — from a game's (and an
  anti-cheat's) point of view it *is* the desktop.

| Known anti-cheat detection vector | Way Vibrance |
|---|---|
| Injecting code/DLLs into the game process | not done — no interaction at all |
| Reading/writing game memory | not done |
| Hooking the game's DirectX/Vulkan calls | not done — shader runs in the compositor |
| Kernel drivers / ring-0 components | none (pure userspace) |
| Modifying game files or binaries | none |
| Overlays on the game window | none |

**Precedent**: driver-level color adjustments (NVIDIA Digital Vibrance,
AMD Saturation, vibrance-gui on Windows) are treated as system settings and
have a long, clean history in competitive play. Wayland simply moves that
same color grading into the compositor because no user app may program the
display's CTM there — the result is visually equivalent and equally inert
with respect to games.

**Honest caveats**: no vendor guarantees third-party software in absolute
terms, and techniques that do sit close to games (ReShade-style in-game
injection, overlays, memory readers) are a different category — this
project is deliberately not one of them. The only game-relevant side effect
here is the documented direct-scanout one: with a window effect active,
exclusive fullscreen may go through the compositor (a small latency cost),
never a detection event.

The footprint is auditable and self-contained: open source, userspace only,
session D-Bus only, no network, no telemetry (see
[Privacy & footprint](#privacy--footprint)).

## Screenshots

| Main window | Profile editor | Tray menu |
|---|---|---|
| ![Main window](screenshots/app.png) | ![Profile editor](screenshots/profiles.png) | ![Tray menu](screenshots/tray.png) |
