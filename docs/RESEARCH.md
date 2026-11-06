[English](RESEARCH.md) | [Português](RESEARCH.pt.md)

# Feasibility research — KWin 6.8 / Wayland (2026-11-05)

The technical notes that grounded the Way Vibrance architecture. Reference
environment: **CachyOS, Plasma 6.3.1, KWin 6.8.2, Qt 6.12, Wayland session**
(`qdbus6 org.kde.KWin /KWin supportInformation`).

> The project was vibe-coded with AI assistance (OpenCode) under the
> maintainer's review — full history in the git log.

## 1. Why "pure vibrance-gui" is impossible on Wayland

- On Windows, vibrance-gui writes the **CTM (Color Transformation Matrix)**
  through the driver API (NVML/ADL) or registers a pipeline filter.
- On Wayland the **compositor is the DRM master**; no user application can
  program the KMS CTM/gamma (which is why `xrandr --set-output-ctm`,
  `vibrant-cli`, etc. do not work). The most that exists outside the
  compositor is KWin's own ICC/HDR *color-management* — not fit for an
  instant global boost.
- Conclusion: the correct place is **inside the compositor**, as a
  post-processing step of the composition. KWin is KDE's compositor →
  **KWin Effect API**.

## 2. Anatomy of effects in KWin 6.8 (verified against the source, `KDE/kwin@master`)

### Loading (src/effect/effectloader.cpp / effecthandler.cpp)

- Three families:
  1. **Compiled-in built-ins**: `kwin_add_builtin_effect(...)` → linked into
     the binary; metadata under `/usr/share/kwin/builtin-effects/*.json` with
     `"org.kde.kwin.effect": { "internal": true }`.
  2. **Scripted** (JS/QML, `KWin/Effect` KPackage type):
     `/usr/share/kwin/effects/<id>/{metadata.json,contents/code/main.js|ui/main.qml}`
     plus, for users, `~/.local/share/kwin/effects/<id>/`
     (KPackage::PackageLoader, `kwin/effects` service).
  3. **Third-party Qt plugins** (our case): `PluginEffectLoader` calls
     `KPluginMetaData::findPlugins("kwin/effects/plugins")` → any `.so` with
     embedded metadata installed under
     `<XDG libdir>/kwin/effects/plugins/` (on Arch:
     `/usr/lib/qt6/plugins/kwin/effects/plugins/` — the directory already
     exists).
- **KWIN_EFFECT_API_VERSION = 0.237** — no binary-compatibility promise;
  rebuild after every KWin update (the norm for the whole category).

### Third-party effect: confirmed plugin format

- `metadata.json`: `KPlugin.{Id, Name, Description, Icon, Category:
  "Appearance", ServiceTypes:["KWin/Effect"], Version, EnabledByDefault:false}`.
- `main.cpp`: `KWIN_EFFECT_FACTORY_SUPPORTED(ClassName, "metadata.json",
  <supported code>)` (macro in `effect/effect.h`; generates an
  `EffectPluginFactory` with IID `org.kde.kwin.EffectPluginFactory<kwinversion>`)
  + `#include "main.moc"`.
- CMake: `find_package(KWin REQUIRED)` → the **`KWin::kwin`** target
  (`/usr/lib/cmake/KWin/KWinConfig.cmake`); install into
  `${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins`.

### The color engine (the central finding)

- `WindowPaintData::setSaturation/multiplySaturation` (1.0 = neutral;
  values > 1 **boost**). In
  `src/scene/itemrenderer_opengl.cpp`:
  - `if (data.saturation() != 1.0) traits |= ShaderTrait::AdjustSaturation;`
  - the `GLShader::FloatUniform::Saturation` uniform is applied **"in linear
    space"** (source comment) — colorwise correct, no extra texture: it uses
    the renderer's existing window-composition shader.
- `requestedEffectChainPosition() == 99` → runs last, over the final look of
  every window.
- `isActive()` false (value 1.0 or disabled) → out of the chain: **zero
  cost** when neutral.
- Inherited limitation: window by window → the **cursor** (hardware plane)
  is not affected. Same trade-off as KWin's own color-blindness-correction
  effect.

### D-Bus

- `org.kde.kwin.Effects` (object `/Effects`): `loadEffect`, `unloadEffect`,
  `toggleEffect`, `isEffectLoaded`, `isEffectSupported`, `reconfigureEffect`,
  `activeEffects` — **hot-load/hot-unload in a live session, no logout
  needed** (confirmed by introspection on KWin 6.8.2). For *rebuilding* the
  .so the session still has to restart (the plugin cannot be safely
  unloaded).
- An effect can register its **own session service** (its code runs inside
  `kwin_wayland`): we use `org.wayvibrance` + `/Vibrance` for live control
  without touching `kwinrc`/`reconfigure` (zero flicker).
- Workspace `windowActivated` signal: basis for per-app profiles (roadmap).

## 2.5 Offscreen path for custom shaders (public API, verified on 6.8)

- `OffscreenEffect` (installed header `effect/offscreeneffect.h`) is KWin's
  own pattern (color-blindness): `redirect(w)` + `setShader(w, shader)` per
  window; **non-redirected windows are forwarded to `effects->drawWindow`
  normally** — i.e. the cost can be toggled per state, and uniform vibrance
  stays active on the normal path. The grab applies `WindowPaintData`
  (saturation included) before the custom shader.
- `ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture,
  QString(), ":/...frag")` → `unique_ptr<GLShader>`; named uniforms via
  `ShaderBinder` + `setUniform("name", QVector3D/float/…)`. All public.
- Plugin `.frag` shaders can `#include "colormanagement.glsl"` (resolved in
  kwin's own qrc, linked into the process) and use
  `sourceEncodingToNitsInDestinationColorspace()` /
  `nitsToDestinationEncoding()` — RGB gains applied in linear space,
  consistent with native vibrance.
- Shader generated **lazily** (only when channels ≠ 1.0; never in the
  constructor if the GL context isn't guaranteed), `setUniform` right after
  creation.

## 2.6 Version tolerance of the C++ side (no numeric pins)

- `Effect/OffscreenEffect::drawWindow`: `void`→`bool` in 6.8 and
  `QRegion`→`Region` in 6.6 — solved with **a single definition**:
  `auto drawWindow(…) -> decltype(effects->drawWindow(…)) override` (the
  public handler evolves in lockstep with the base; `return <void-expr>` is
  valid in a `cv void` function). Region parameter type via `WvPaintRegion`
  keyed on `KWIN_EFFECT_API_VERSION_MINOR >= 237`.
- `&OffscreenEffect::drawWindow` (member pointer) **cannot** be used to
  extract the return type inside the derived class body — GCC treats it as
  a protected access to an incomplete type; the public-object variant
  (`effects->`) compiles.

### "Primary monitor" scoping (0.3)

> **UPDATE (0.5):** the `activeScreen()` proxy below was replaced — the mode
> now targets a **user-picked output** (`PrimaryOutput` in kwinrc, tray
> dropdown / `wayvibrance primary output <name>`), matched by connector name
> over `effects->screens()` with a hotplug-invalidated cache; "automatic" =
> first output. Kept here as the historical record of the original research.

- The public effects API **exposes no "primary output" concept** (nothing
  like `primaryScreen()` in the installed `options.h`/`effecthandler.h`).
  The proxy used: `EffectsHandler::activeScreen()` — the output under the
  cursor — public and stable, which in single-"primary"-monitor gaming
  setups matched what users expected.
- Scoping is decided **per window, every `drawWindow`** (`w->screen()`), so
  windows moving between screens enter/leave the effect naturally without
  needing repositioning signals (nonexistent in the public 6.8 API). Out of
  scope: full bypass (neither uniform nor offscreen). The `redirect()` set
  (RGB gains) stayed global — textures of out-of-scope windows sat idle and
  the base discarded/redrew them.

## 3. Alternatives mapped and rejected

| Path | Verdict |
|---|---|
| Direct KMS CTM (libdrm) | ❌ impossible on Wayland (exclusive DRM master) |
| JS/QML effect (scripting `ShaderTrait.AdjustSaturation`) | ⚠️ viable but grab-based (extra textures), weak live control (reconfigure), no own D-Bus service |
| `OffscreenEffect` with custom GLSL (via kwin-daltonismo) | ⚠️ heavier (per-window grab) — now used **only and conditionally** for RGB gains (see §2.5); a future *true vibrance* mode (skin-safe) may move it to a scene pass |
| `multiplySaturation` on the built-in renderer | ✅ **chosen** — linear, no extra pass, zero cost at neutral |
| Porting to other compositors | ❌ out of scope — the project is **KDE/Plasma exclusive** |

## 3.1 Version-compatibility strategy (project requirement)

- **No Plasma/KWin version number is pinned in code.** The only gate is
  behavioral: `supported()` checks OpenGL compositing at runtime, and a
  floor `#error` requires just `KWIN_EFFECT_API_VERSION_MINOR >= 236` (what
  the API has exposed since KWin 6.0).
- Tray and CLI use **D-Bus only** (`org.wayvibrance`,
  `org.kde.kwin.Effects`) — stable across all of Plasma 6, zero pins.
- The plugin is compiled against the installed KWin (`find_package(KWin
  REQUIRED)` with no version ceiling). The public API we use
  (`WindowPaintData::multiplySaturation`, `drawWindow`,
  `KWIN_EFFECT_FACTORY_SUPPORTED`, `requestedEffectChainPosition`) exists
  since KWin 6.0 and does not evolve destructively within Plasma 6 — a
  `scripts/build.sh` rebuild is the only step needed after updates.

### `drawWindow` signature mapping (verified against the source, per release)

| KWin | effects API minor | 5th parameter | return |
|---|---|---|---|
| 6.0 – 6.5 | 236 | `QRegion` | `void` |
| 6.6 – 6.7 | 237 | `KWin::Region` | `void` |
| 6.8 | 237 | `KWin::Region` | `bool` |
| master (6.9-git) | 237 | `KWin::Region` | `bool` |

The effect absorbs both changes without version pins:

- **Region type**: `using WvPaintRegion = Region` when
  `KWIN_EFFECT_API_VERSION_MINOR >= 237`, else `QRegion` (keyed on the macro
  the header itself defines, not a release guess).
- **Return type**: declared as
  `auto drawWindow(...) -> decltype(effects->drawWindow(...))`. The deduced
  type is `void` on 6.0–6.7 and `bool` on 6.8+, matching the base. Since
  `return <void-expr>` is valid inside a `cv void` function, **one single
  definition** compiles across the whole range.

## 4. References

- vibrance-gui (x64christ/Vibrance-GU) and VibranceGUI (vibrancegui.com) —
  the parity model (driver API + profiles + hotkey + tray).
- libvibrant / vibrant-cli — skin-tone-preserving vibrance algorithm for a
  future mode.
- EfeDursun125/kwin-saturation — C++ effect using `multiplySaturation` +
  GUI; confirmed the built-in uniform technique.
- Garward/kwin-saturation-effect — custom per-grab GLSL shader; documents
  the CTM impossibility on Wayland; 2.0 ≈ NV "1023".
- KDE/kwin `src/effect/effectloader.cpp`,
  `src/scene/itemrenderer_opengl.cpp`, `src/plugins/colorblindnesscorrection/*`
  — the architecture above.
- develop.kde.org/docs/plasma/kwin/api — scripting API (ShaderTrait etc.).

## 5. Scale numbers

- Factor 1.0 = neutral (Windows: NV 576/1023 ≈ 100%).
- 2.0 ≈ the maximum driver-equivalent vibrance (NV 1023).
- The tray UI exposes 0–200% on every slider (internal clamp [0, 2] in the
  effect — 2.0 is the real ceiling of KWin's color chain; 0 = grayscale).
