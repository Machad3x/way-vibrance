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
5. **Prebuilt packages** — in progress for the next release, produced by
   `scripts/package.sh` in per-distro containers (matching each distro's
   KWin/Qt ABI) and attached to the GitHub release:
   - RPM → Fedora, and the supported path for **Fedora Atomic/Kinoite**
     (`rpm-ostree install way-vibrance-….rpm`) — verified: KWin 6.3–6.8 only
     load effects from system plugin directories, so user-scope installs are
     not an option for the effect;
   - DEB → Debian 13+ / Ubuntu 24.10+ / KDE Neon / Mint;
   - PKGBUILD → Arch (AUR-style auto-rebuild so every KWin update
     recompiles the effect).
6. **Sound/OSD on profile switch** (vibrance-gui parity) — layer-shell OSD.
7. CI: run the podman matrix (arch/fedora/debian/ubuntu) automatically on
   pushes and publish the artifacts on release tags.

### Deliberately out of scope
- **SteamOS gaming session**: the gamescope session runs no KWin, so a KWin
  effect cannot color its games; the effect applies to the SteamOS desktop
  session only.
- **Flatpak**: sandboxed apps cannot load host-KWin plugins.
