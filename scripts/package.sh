#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build AND verify prebuilt packages, one per distro release, each built in
# a throwaway container of that release so the binaries match its KWin/Qt
# ABI (the effects API has no cross-version binary guarantee):
#
#   dist/way-vibrance-<ver>-1.fc<NN>.x86_64.rpm     Fedora 44, 45, ...
#         (also the rpm-ostree install path for Silverblue/Kinoite)
#   dist/way-vibrance_<ver>~debian13_amd64.deb      Debian 13 stable
#   dist/way-vibrance_<ver>~debian14_amd64.deb      Debian 14 testing
#   dist/way-vibrance_<ver>~ubuntu26.04_amd64.deb   Ubuntu 26.04/26.10 (supported releases only)
#   dist/way-vibrance-<ver>-1-x86_64.pkg.tar.zst    Arch (rolling)
#
# After every build, a FRESH container of the same release installs the
# artifact with the native package manager and verifies:
#   1. dependency resolution succeeds (dnf/apt/pacman -U),
#   2. the expected files are placed,
#   3. 'wayvibrance --help' runs,
#   4. the effect plugin dlopens cleanly with the release's libkwin
#      preloaded — the real ABI smoke test,
#   5. the tray starts under the offscreen platform (all GUI/KF6 runtime
#      deps satisfied) and survives 3 s under its own event loop.
#
# Targets build in parallel (default up to 4, each capped to a fair share
# of the CPUs) with per-target logs under dist/logs/. Requires podman (or
# CONTAINER_TOOL=docker). Recipes live in packaging/; version comes from
# CMakeLists.txt. Missing images are pulled; unfetchable releases are
# skipped with a warning.
#
# Usage: scripts/package.sh [target...]   (default: the full matrix below)

set -uo pipefail
cd "$(dirname "$0")/.."

VERSION="$(grep -m1 'project(way-vibrance VERSION' CMakeLists.txt | awk '{print $3}')"
DATE_R="$(date -R)"
DIST="$PWD/dist"
LOGDIR="$DIST/logs"
TOOL="${CONTAINER_TOOL:-podman}"
NCPU="$(nproc)"
JOBS="${PACKAGE_JOBS:-$(( NCPU / 4 > 0 ? NCPU / 4 : 1 ))}"
JBERS="${PACKAGE_JOBS_PER_BUILD:-$(( NCPU / JOBS ))}"
# Policy: packages are built and supported ONLY for distro releases that
# are still supported upstream (Ubuntu 25.04/25.10 reached EOL and were
# dropped; Fedora ships N+1/N, Debian stable + testing, Arch rolling).
DEFAULT_TARGETS=(fedora-44 fedora-45 debian-13 debian-14
                 ubuntu-26.04 ubuntu-26.10 arch)

# Private, unpredictable work root (mktemp refuses to reuse an existing
# path, closing the /tmp collision and symlink-planting windows that a
# predictable /tmp/wv-*-$$ name would leave open).
WORKROOT="$(mktemp -d "${TMPDIR:-/tmp}/way-vibrance-package.XXXXXXXX")" \
    || { echo "==> could not create private work dir" >&2; exit 1; }
PRIST="$WORKROOT/pristine"
STAGE_ROOT="$WORKROOT/stage"
trap 'chmod -R u+rwX "$WORKROOT" 2>/dev/null; rm -rf "$WORKROOT"' EXIT

if ! git diff --quiet HEAD 2>/dev/null; then
    echo "==> NOTE: working tree differs from HEAD; the source tarball is HEAD." >&2
fi

mkdir -p "$DIST" "$LOGDIR"
echo "==> Staging way-vibrance $VERSION from HEAD"
mkdir -p "$PRIST"
git archive --format=tar --prefix="way-vibrance-$VERSION/" HEAD | tar -x -C "$PRIST"

have_image() {
    local img="docker.io/library/$1"
    "$TOOL" image exists "$img" || "$TOOL" pull -q "$img" >/dev/null 2>&1 || return 1
}

# Common verification snippet, run inside a fresh container of the target
# release after the package was installed with its native manager. The
# $ARTIFACT env names the installed package files; paths are probed from
# the package manager itself (never hardcoded per distro).
verify_installed() { # writes into /work/verify.log inside the container
    cat <<'VEOF'
set -e
case "$PM" in
  rpm) list_pkgs="rpm -ql way-vibrance" ;;
  apt) list_pkgs="dpkg -L way-vibrance" ;;
  pkg) list_pkgs="pacman -Ql way-vibrance | awk '{print \$2}'" ;;
esac
echo "-- files --"
eval "$list_pkgs" | sort | head -20
PLUGIN=$(eval "$list_pkgs" | grep -m1 'kwin/effects/plugins/wayvibrance.so$' || true)
[ -n "$PLUGIN" ] && [ -f "$PLUGIN" ] || { echo "FAIL: plugin not installed"; exit 1; }
command -v wayvibrance      >/dev/null || { echo "FAIL: cli missing"; exit 1; }
command -v wayvibrance-tray >/dev/null || { echo "FAIL: tray missing"; exit 1; }
echo "-- cli --help --"
wayvibrance --help >/dev/null
echo "-- dlopen plugin against this release's libkwin --"
LIBKWIN="$(ldconfig -p | grep -m1 -oE '/[^ ]*libkwin\.so[^ ]*')"
[ -n "$LIBKWIN" ] || { echo "FAIL: libkwin not found"; exit 1; }
python3 - "$LIBKWIN" "$PLUGIN" <<'PY'
import ctypes, sys
ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_GLOBAL)
h = ctypes.CDLL(sys.argv[2])          # resolves every symbol now
print("dlopen-ok:", bool(h))
PY
echo "-- tray boot (offscreen, dbus session, 3s) --"
DBUSRUN="$(command -v dbus-run-session || echo /usr/sbin/dbus-run-session)"
mkdir -p /tmp/xdg && chmod 700 /tmp/xdg
XDG_RUNTIME_DIR=/tmp/xdg "$DBUSRUN" -- env QT_QPA_PLATFORM=offscreen timeout 3 wayvibrance-tray >/tmp/tray.out 2>&1 &
TRAY=$!
sleep 3.5
if kill -0 $TRAY 2>/dev/null; then kill -9 $TRAY; echo "tray-alive-ok";
else wait $TRAY && { echo "FAIL: tray exited cleanly too early"; exit 1; }; fi
echo "VERIFY-OK"
VEOF
}

pkg_fedora() { # $1 = release number
    local target="fedora-$1" rel="$1" stage
    have_image "fedora:$rel" || { echo "  [SKIP] $target (image unavailable)"; return 0; }
    stage="$STAGE_ROOT/$target"; mkdir -p "$stage"; cp -a "$PRIST/way-vibrance-$VERSION" "$stage/"
    sed "s/@VERSION@/$VERSION/g" packaging/way-vibrance.spec > "$stage/way-vibrance.spec"
    echo "  [build ] $target"
    "$TOOL" run --rm -v "$stage:/work:rw" -v "wv-dnf-$target":/var/cache/dnf -e RPM_BUILD_NCPUS="$JBERS" \
        "docker.io/library/fedora:$rel" bash -c "
        dnf -q -y install rpm-build cmake extra-cmake-modules gcc-c++ kwin-devel \
            kf6-kconfig-devel kf6-kcoreaddons-devel kf6-kdbusaddons-devel \
            kf6-kglobalaccel-devel qt6-qtbase-devel libepoxy-devel vulkan-headers \
            libdrm-devel wayland-devel >/tmp/dnf.log 2>&1 || { tail -3 /tmp/dnf.log; exit 1; }
        mkdir -p /root/rpmbuild/{BUILD,RPMS,SOURCES,SPECS}
        tar czf /root/rpmbuild/SOURCES/way-vibrance-$VERSION.tar.gz -C /work way-vibrance-$VERSION
        rpmbuild -bb /work/way-vibrance.spec >/tmp/rpmbuild.log 2>&1 || { tail -20 /tmp/rpmbuild.log; exit 1; }
        cp /root/rpmbuild/RPMS/*/way-vibrance-$VERSION-*.rpm /work/
    " > "$LOGDIR/$target.build.log" 2>&1 \
      || { echo "  [FAIL  ] $target build (see $LOGDIR/$target.build.log)"; return 1; }
    mv -f "$stage"/way-vibrance-"$VERSION"-1.fc*.rpm "$DIST"/
    local rpm_name
    rpm_name="$(ls "$DIST" | grep "fc$rel" | tail -1)"
    echo "  [verify] $target (install $rpm_name in fresh fedora:$rel)"
    mkdir -p "$stage/vout"; verify_installed > "$stage/vout/verify.sh"
    "$TOOL" run --rm -v "$stage:/work:ro" -v "$stage/vout:/work/vout:rw" -v "$DIST:/dist:ro" -v "wv-dnf-$target":/var/cache/dnf \
        -e PM=rpm \
        "docker.io/library/fedora:$rel" bash -c "
        set -e
        dnf -q -y install /dist/$rpm_name python3 dbus dbus-tools >/tmp/i.log 2>&1 || { tail -15 /tmp/i.log; exit 1; }
        bash /work/vout/verify.sh 2>&1 | tee /work/vout/verify.out | tail -20
        grep -q VERIFY-OK /work/vout/verify.out
    " > "$LOGDIR/$target.verify.log" 2>&1 \
      && echo "  [OK    ] $target" \
      || { echo "  [FAIL  ] $target verify (see $LOGDIR/$target.verify.log)"; return 1; }
}

pkg_deb() { # $1=target $2=image $3=label $4=suffix
    local target="$1" img="$2" label="$3" suf="$4" stage
    have_image "$img" || { echo "  [SKIP] $target (image unavailable)"; return 0; }
    stage="$STAGE_ROOT/$target"; mkdir -p "$stage"
    cp -a "$PRIST/way-vibrance-$VERSION" "$stage/"
    cp -r packaging/debian "$stage/way-vibrance-$VERSION/debian"
    sed -i "s/@VERSION@/$VERSION$suf/g; s/@DATE@/$DATE_R/g" "$stage/way-vibrance-$VERSION/debian/changelog"
    echo "  [build ] $target ($label)"
    "$TOOL" run --rm -v "$stage:/work:rw" -v "wv-apt-$target":/var/cache/apt \
        -e DEB_BUILD_OPTIONS="parallel=$JBERS" -e LANG=C.UTF-8 \
        "docker.io/library/$img" bash -c "
        export DEBIAN_FRONTEND=noninteractive
        apt-get -qq update >/dev/null 2>&1
        apt-get -qq install -y --no-install-recommends dpkg-dev debhelper build-essential cmake \
            extra-cmake-modules qt6-base-dev kwin-dev libkf6config-dev libkf6coreaddons-dev \
            libkf6dbusaddons-dev libkf6globalaccel-dev libepoxy-dev libvulkan-dev libdrm-dev \
            libwayland-dev pkg-config >/tmp/apt.log 2>&1 || { tail -5 /tmp/apt.log; exit 1; }
        cd '/work/way-vibrance-$VERSION'
        dpkg-buildpackage -us -uc -b >/tmp/dpkg.log 2>&1 || { tail -25 /tmp/dpkg.log; exit 1; }
    " > "$LOGDIR/$target.build.log" 2>&1 \
      || { echo "  [FAIL  ] $target build (see $LOGDIR/$target.build.log)"; return 1; }
    mv -f "$stage"/way-vibrance_"${VERSION}${suf}"_amd64.deb "$DIST"/
    local deb_name="way-vibrance_${VERSION}${suf}_amd64.deb"
    echo "  [verify] $target (install $deb_name in fresh $img)"
    mkdir -p "$stage/vout"; verify_installed > "$stage/vout/verify.sh"
    "$TOOL" run --rm -v "$stage:/work:ro" -v "$stage/vout:/work/vout:rw" -v "$DIST:/dist:ro" -v "wv-apt-$target":/var/cache/apt \
        -e PM=apt -e DEBIAN_FRONTEND=noninteractive \
        "docker.io/library/$img" bash -c "
        set -e
        apt-get -qq update >/dev/null 2>&1 || true
        apt-get -qq install -y --no-install-recommends python3 >/tmp/py.log 2>&1 || true
        apt-get install -y --no-install-recommends \"/dist/$deb_name\" >/tmp/i.log 2>&1 || { tail -15 /tmp/i.log; exit 1; }
        bash /work/vout/verify.sh 2>&1 | tee /work/vout/verify.out | tail -20
        grep -q VERIFY-OK /work/vout/verify.out
    " > "$LOGDIR/$target.verify.log" 2>&1 \
      && echo "  [OK    ] $target" \
      || { echo "  [FAIL  ] $target verify (see $LOGDIR/$target.verify.log)"; return 1; }
}

pkg_arch() {
    local target=arch stage
    have_image archlinux:latest || { echo "  [SKIP] $target (image unavailable)"; return 0; }
    stage="$STAGE_ROOT/$target"; mkdir -p "$stage/way-vibrance"
    sed "s/@VERSION@/$VERSION/g" packaging/PKGBUILD > "$stage/way-vibrance/PKGBUILD"
    tar czf "$stage/way-vibrance/way-vibrance-$VERSION.tar.gz" -C "$PRIST" "way-vibrance-$VERSION"
    echo "  [build ] $target"
    "$TOOL" run --rm -v "$stage:/work:rw" -e MAKEFLAGS="-j$JBERS" \
        docker.io/library/archlinux:latest bash -c "
        pacman -Sy --noconfirm >/dev/null 2>&1
        pacman -S --noconfirm --needed base-devel cmake extra-cmake-modules kwin qt6-base \
            kconfig kcoreaddons kdbusaddons kglobalaccel vulkan-headers >/tmp/pacman.log 2>&1 || { tail -5 /tmp/pacman.log; exit 1; }
        # makepkg refuses to run as root; build inside the unprivileged
        # user's home so nothing it creates lands on the bind mount except
        # the final artifact copy — rootless /tmp cleanup stays painless.
        useradd -m -u 1000 builder
        chmod -R a+rwX /work
        su builder -s /bin/bash -c 'mkdir -p ~/b && cp /work/way-vibrance/PKGBUILD /work/way-vibrance/*.tar.gz ~/b/ && cd ~/b && makepkg -scf --noconfirm' >/tmp/makepkg.log 2>&1 || { tail -25 /tmp/makepkg.log; exit 1; }
        cp /home/builder/b/way-vibrance-$VERSION-1-x86_64.pkg.tar.zst /work/
    " > "$LOGDIR/$target.build.log" 2>&1 \
      || { echo "  [FAIL  ] $target build (see $LOGDIR/$target.build.log)"; return 1; }
    mv -f "$stage"/way-vibrance-"$VERSION"-1-x86_64.pkg.tar.zst "$DIST"/
    local pkg_name="way-vibrance-$VERSION-1-x86_64.pkg.tar.zst"
    echo "  [verify] $target (install $pkg_name in fresh arch)"
    mkdir -p "$stage/vout"; verify_installed > "$stage/vout/verify.sh"
    "$TOOL" run --rm -v "$stage:/work:ro" -v "$stage/vout:/work/vout:rw" -v "$DIST:/dist:ro" \
        -e PM=pkg \
        docker.io/library/archlinux:latest bash -c "
        set -e
        pacman -Sy --noconfirm >/dev/null 2>&1
        pacman -S --noconfirm --needed python3 dbus >/tmp/py.log 2>&1 || true
        pacman -U --noconfirm \"/dist/$pkg_name\" >/tmp/i.log 2>&1 || { tail -15 /tmp/i.log; exit 1; }
        bash /work/vout/verify.sh 2>&1 | tee /work/vout/verify.out | tail -20
        grep -q VERIFY-OK /work/vout/verify.out
    " > "$LOGDIR/$target.verify.log" 2>&1 \
      && echo "  [OK    ] $target" \
      || { echo "  [FAIL  ] $target verify (see $LOGDIR/$target.verify.log)"; return 1; }
}

dispatch() { # $1 = target
    case "$1" in
        fedora-*)  pkg_fedora "${1#fedora-}" ;;
        debian-13) pkg_deb debian-13 debian:trixie-slim "Debian 13 stable" "~debian13" ;;
        debian-14) local img
                   if have_image debian:forky; then img=debian:forky
                   elif have_image debian:testing; then img=debian:testing
                   else echo "  [SKIP] debian-14 (image unavailable)"; return 0; fi
                   pkg_deb debian-14 "$img" "Debian 14 testing" "~debian14" ;;
        ubuntu-*)  pkg_deb "$1" "ubuntu:${1#ubuntu-}" "Ubuntu ${1#ubuntu-}" "~ubuntu${1#ubuntu-}" ;;
        arch)      pkg_arch ;;
        *) echo "unknown target: $1" >&2; return 1 ;;
    esac
}

TARGETS=("$@"); ((${#TARGETS[@]})) || TARGETS=("${DEFAULT_TARGETS[@]}")
echo "==> ${#TARGETS[@]} targets: build+verify, $JOBS at a time ($JBERS CPUs each, $NCPU total)"
PIDS=()
for t in "${TARGETS[@]}"; do
    while (( ${#PIDS[@]} >= JOBS )); do
        alive=()
        for p in "${PIDS[@]}"; do kill -0 "$p" 2>/dev/null && alive+=("$p"); done
        PIDS=("${alive[@]}")
        (( ${#PIDS[@]} < JOBS )) && break
        sleep 1
    done
    dispatch "$t" &
    PIDS+=($!)
done
RC=0
for p in "${PIDS[@]}"; do wait "$p" || RC=1; done

echo
echo "==> Artifacts in dist/:"
ls -la "$DIST" | grep -vE "^total|^d.* logs$"
exit $RC
