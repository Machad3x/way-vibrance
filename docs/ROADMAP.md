[English](ROADMAP.md) | [Português](ROADMAP.pt.md)

# Way Vibrance — Roadmap

Extracted from the README; the [README](../README.md) keeps only a summary.
Back to [README](../README.md) · [Features](FEATURES.md).

1. **"True" vibrance** — protects skin tones ([libvibrant](https://github.com/libvibrant/vibrant-cli)
   algorithm) as an optional shader mode.
2. **Per-output values** — each monitor gets its own vibrance/gains (the
   monitor *picker* is already shipped since v0.5.0; per-output *values*
   need a per-output uniform or scene pass).
3. **HDR / wide-gamut correctness** — BT.2020-aware grading.
4. **Vulkan compositing** — apply the saturation/gains under KWin's Vulkan
   backend (today the effect intentionally declines to load there instead
   of silently no-op).
5. **Prebuilt packages** — shipped with the next release via
   `scripts/package.sh`, **one artifact per distro release** (the effect is
   compiled against that release's KWin; the effects API has no
   cross-version binary guarantee):
   - RPM → Fedora 44, 45; on **Fedora Atomic/Kinoite** install with
     `rpm-ostree install` — verified: KWin 6.3–6.8 only loads effects from
     system plugin directories, so user-scope installs are not an option
     for the effect;
   - DEB → Debian 13 + 14 (testing) and Ubuntu 26.04/26.10 — supported
     releases only; EOL releases are dropped (25.04/25.10 were removed once
     Canonical ended support);
   - PKGBUILD → Arch (AUR-style auto-rebuild so every KWin update
     recompiles the effect).
   CI: the podman matrix runs every build; a GitHub Action could publish
   `dist/` automatically on release tags.
6. **Sound/OSD on profile switch** (vibrance-gui parity) — layer-shell OSD.
7. CI: run the podman matrix (arch/fedora/debian/ubuntu) automatically on
   pushes and publish the artifacts on release tags.

### Deliberately out of scope
- **SteamOS gaming session**: the gamescope session runs no KWin, so a KWin
  effect cannot color its games; the effect applies to the SteamOS desktop
  session only.
- **Flatpak**: sandboxed apps cannot load host-KWin plugins.
