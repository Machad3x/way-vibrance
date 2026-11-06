#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Produce prebuilt packages for every supported family, each in a throwaway
# container of that distro (so binaries match its KWin/Qt ABI) — the flow:
#
#   dist/way-vibrance-<ver>-1.fc<xx>.x86_64.rpm        (Fedora + rpm-ostree/Atomic)
#   dist/way-vibrance_<ver>_amd64.deb                  (Debian 13, Ubuntu 25.04+)
#   dist/way-vibrance-<ver>-1-x86_64.pkg.tar.zst       (Arch & derivatives)
#
# Requires podman (or set CONTAINER_TOOL=docker). Package recipes live in
# packaging/; the version comes from CMakeLists.txt. Attach the artifacts to
# the GitHub release afterwards.
#
# Usage: scripts/package.sh [fedora|debian|ubuntu|arch]...   (default: all)

set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="$(grep -m1 'project(way-vibrance VERSION' CMakeLists.txt | awk '{print $3}')"
DATE_R="$(date -R)"
DIST="$PWD/dist"
TOOL="${CONTAINER_TOOL:-podman}"
KEEP_STAGE="${KEEP_STAGE:-0}"
if [ "$KEEP_STAGE" = 1 ] && [ -d /tmp/wv-stage ]; then
    STAGE=/tmp/wv-stage
    echo "==> Reusing /tmp/wv-stage (KEEP_STAGE=1)"
else
    STAGE="$(mktemp -d /tmp/wv-stage.XXXX)"
    trap 'rm -rf "$STAGE"' EXIT
fi

if ! git diff --quiet HEAD 2>/dev/null; then
    echo "==> WARNING: working tree differs from HEAD; the source tarball is taken from HEAD." >&2
fi

mkdir -p "$DIST"
if (($# == 0)); then
    set -- fedora debian ubuntu arch
fi
echo "==> Staging way-vibrance $VERSION from HEAD"
git archive --format=tar --prefix="way-vibrance-$VERSION/" HEAD | tar -x -C "$STAGE"

pkg_fedora() {
    echo "==> RPM (Fedora)"
    sed "s/@VERSION@/$VERSION/g" packaging/way-vibrance.spec > "$STAGE/way-vibrance.spec"
    "$TOOL" run --rm -v "$STAGE:/work:rw" -v wv-dnf:/var/cache/dnf docker.io/library/fedora:latest bash -c "
        dnf -q -y install rpm-build cmake extra-cmake-modules gcc-c++ kwin-devel \
            kf6-kconfig-devel kf6-kcoreaddons-devel kf6-kdbusaddons-devel \
            kf6-kglobalaccel-devel qt6-qtbase-devel libepoxy-devel vulkan-headers \
            libdrm-devel wayland-devel >/tmp/dnf.log 2>&1 || { tail -3 /tmp/dnf.log; exit 1; }
        mkdir -p /root/rpmbuild/{BUILD,RPMS,SOURCES,SPECS}
        tar czf /root/rpmbuild/SOURCES/way-vibrance-$VERSION.tar.gz -C /work way-vibrance-$VERSION
        rpmbuild -bb --define '_topdir /root/rpmbuild' /work/way-vibrance.spec >/tmp/rpmbuild.log 2>&1 || { tail -20 /tmp/rpmbuild.log; exit 1; }
        cp /root/rpmbuild/RPMS/*/way-vibrance-$VERSION-*.rpm /work/
    "
    mv -f "$STAGE"/way-vibrance-"$VERSION"-*.rpm "$DIST"/ && echo "    $(ls "$DIST" | grep rpm | tail -1)"
}

pkg_deb() { # $1=image $2=label
    echo "==> DEB ($2)"
    rm -rf "$STAGE/way-vibrance-$VERSION/debian"
    cp -r packaging/debian "$STAGE/way-vibrance-$VERSION/debian"
    sed -i "s/@VERSION@/$VERSION/g; s/@DATE@/$DATE_R/g" "$STAGE/way-vibrance-$VERSION/debian/changelog"
    "$TOOL" run --rm -v "$STAGE:/work:rw" -v wv-aptcache:/var/cache/apt "docker.io/library/$1" bash -c "
        export DEBIAN_FRONTEND=noninteractive
        apt-get -qq update >/dev/null 2>&1
        apt-get -qq install -y --no-install-recommends dpkg-dev debhelper build-essential cmake \
            extra-cmake-modules qt6-base-dev kwin-dev libkf6config-dev libkf6coreaddons-dev \
            libkf6dbusaddons-dev libkf6globalaccel-dev libepoxy-dev libvulkan-dev libdrm-dev \
            libwayland-dev pkg-config >/tmp/apt.log 2>&1 || { tail -3 /tmp/apt.log; exit 1; }
        cd '/work/way-vibrance-$VERSION'
        dpkg-buildpackage -us -uc -b >/tmp/dpkg.log 2>&1 || { tail -25 /tmp/dpkg.log; exit 1; }
        ls /work/*.deb
    "
    mv -f "$STAGE"/way-vibrance_*_amd64.deb "$DIST"/ && echo "    $(ls "$DIST" | grep deb | tail -1)"
}

pkg_arch() {
    echo "==> PKG (Arch)"
    mkdir -p "$STAGE/way-vibrance"
    sed "s/@VERSION@/$VERSION/g" packaging/PKGBUILD > "$STAGE/way-vibrance/PKGBUILD"
    tar czf "$STAGE/way-vibrance/way-vibrance-$VERSION.tar.gz" -C "$STAGE" "way-vibrance-$VERSION"
    "$TOOL" run --rm -v "$STAGE:/work:rw" docker.io/library/archlinux:latest bash -c "
        pacman -Sy --noconfirm >/dev/null 2>&1
        pacman -S --noconfirm --needed base-devel cmake extra-cmake-modules kwin qt6-base \
            kconfig kcoreaddons kdbusaddons kglobalaccel vulkan-headers >/tmp/pacman.log 2>&1 || { tail -5 /tmp/pacman.log; exit 1; }
        cd /work/way-vibrance
        makepkg -scf --noconfirm >/tmp/makepkg.log 2>&1 || { tail -25 /tmp/makepkg.log; exit 1; }
        cp way-vibrance-$VERSION-1-x86_64.pkg.tar.zst /work/
    "
    mv -f "$STAGE"/way-vibrance-"$VERSION"-1-x86_64.pkg.tar.zst "$DIST"/ && echo "    $(ls "$DIST" | grep zst | tail -1)"
}

TARGETS=("${@:-fedora debian ubuntu arch}")
for t in "${TARGETS[@]}"; do
    case "$t" in
        fedora) pkg_fedora ;;
        debian) pkg_deb debian:trixie-slim "Debian 13" ;;
        ubuntu) pkg_deb ubuntu:25.04 "Ubuntu 25.04" ;;
        arch)   pkg_arch ;;
        *) echo "unknown target: $t (use fedora|debian|ubuntu|arch)" >&2; exit 1 ;;
    esac
done

echo
echo "==> Artifacts in dist/:"
ls -la "$DIST"
