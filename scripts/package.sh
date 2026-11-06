#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Produce prebuilt packages, each in a throwaway container of the target
# distro release so the binaries match that release's KWin/Qt ABI (the
# effects API has no cross-version binary guarantee, so one file per
# release — this script makes that cheap):
#
#   dist/way-vibrance-<ver>-1.fc<NN>.x86_64.rpm       Fedora 44, 45, ...
#         (also the rpm-ostree install path for Silverblue/Kinoite)
#   dist/way-vibrance_<ver>~debian13_amd64.deb        Debian 13 (stable)
#   dist/way-vibrance_<ver>~debian14_amd64.deb        Debian 14 (testing)
#   dist/way-vibrance_<ver>~ubuntu25.04_amd64.deb     Ubuntu 25.04/25.10/26.04/...
#   dist/way-vibrance-<ver>-1-x86_64.pkg.tar.zst      Arch (rolling)
#
# Requires podman (or CONTAINER_TOOL=docker). Recipes live in packaging/;
# version comes from CMakeLists.txt. Missing images are pulled; unfetchable
# releases are skipped with a warning. Attach the artifacts to the GitHub
# release afterwards.
#
# Usage: scripts/package.sh [target...]   (default: the full matrix below)

set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="$(grep -m1 'project(way-vibrance VERSION' CMakeLists.txt | awk '{print $3}')"
DATE_R="$(date -R)"
DIST="$PWD/dist"
TOOL="${CONTAINER_TOOL:-podman}"
KEEP_STAGE="${KEEP_STAGE:-0}"
DEFAULT_TARGETS=(fedora-44 fedora-45 debian-13 debian-14
                 ubuntu-25.04 ubuntu-25.10 ubuntu-26.04 ubuntu-26.10 arch)

if [ "$KEEP_STAGE" = 1 ] && [ -d /tmp/wv-stage ]; then
    STAGE=/tmp/wv-stage
    echo "==> Reusing /tmp/wv-stage (KEEP_STAGE=1)"
else
    STAGE="$(mktemp -d /tmp/wv-stage.XXXX)"
    trap 'chmod -R u+rwX "$STAGE" 2>/dev/null; rm -rf "$STAGE"' EXIT
fi

if ! git diff --quiet HEAD 2>/dev/null; then
    echo "==> WARNING: working tree differs from HEAD; the source tarball is taken from HEAD." >&2
fi

mkdir -p "$DIST"
if [ "$KEEP_STAGE" != 1 ] || [ ! -d "$STAGE/way-vibrance-$VERSION" ]; then
    echo "==> Staging way-vibrance $VERSION from HEAD"
    git archive --format=tar --prefix="way-vibrance-$VERSION/" HEAD | tar -x -C "$STAGE"
fi

have_image() { # pulls if absent; returns non-zero when unavailable
    local img="docker.io/library/$1"
    "$TOOL" image exists "$img" || "$TOOL" pull -q "$img" >/dev/null 2>&1 || return 1
}

pkg_fedora() { # $1 = release number
    local rel="$1"
    have_image "fedora:$rel" || { echo "==> SKIP fedora-$rel (no image)"; return; }
    echo "==> RPM (Fedora $rel)"
    sed "s/@VERSION@/$VERSION/g" packaging/way-vibrance.spec > "$STAGE/way-vibrance.spec"
    "$TOOL" run --rm -v "$STAGE:/work:rw" -v wv-dnf:/var/cache/dnf "docker.io/library/fedora:$rel" bash -c "
        dnf -q -y install rpm-build cmake extra-cmake-modules gcc-c++ kwin-devel \
            kf6-kconfig-devel kf6-kcoreaddons-devel kf6-kdbusaddons-devel \
            kf6-kglobalaccel-devel qt6-qtbase-devel libepoxy-devel vulkan-headers \
            libdrm-devel wayland-devel >/tmp/dnf.log 2>&1 || { tail -3 /tmp/dnf.log; exit 1; }
        mkdir -p /root/rpmbuild/{BUILD,RPMS,SOURCES,SPECS}
        tar czf /root/rpmbuild/SOURCES/way-vibrance-$VERSION.tar.gz -C /work way-vibrance-$VERSION
        rpmbuild -bb /work/way-vibrance.spec >/tmp/rpmbuild.log 2>&1 || { tail -20 /tmp/rpmbuild.log; exit 1; }
        cp /root/rpmbuild/RPMS/*/way-vibrance-$VERSION-*.rpm /work/
    "
    mv -f "$STAGE"/way-vibrance-"$VERSION"-1.fc*.rpm "$DIST"/ && echo "    $(ls "$DIST" | grep "fc$rel" | tail -1)"
}

pkg_deb() { # $1=image $2=label $3=version-suffix
    have_image "$1" || { echo "==> SKIP $2 (no image)"; return; }
    echo "==> DEB ($2)"
    rm -rf "$STAGE/way-vibrance-$VERSION/debian"
    cp -r packaging/debian "$STAGE/way-vibrance-$VERSION/debian"
    sed -i "s/@VERSION@/$VERSION$3/g; s/@DATE@/$DATE_R/g" "$STAGE/way-vibrance-$VERSION/debian/changelog"
    "$TOOL" run --rm -v "$STAGE:/work:rw" -v wv-aptcache:/var/cache/apt "docker.io/library/$1" bash -c "
        export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
        apt-get -qq update >/dev/null 2>&1
        apt-get -qq install -y --no-install-recommends dpkg-dev debhelper build-essential cmake \
            extra-cmake-modules qt6-base-dev kwin-dev libkf6config-dev libkf6coreaddons-dev \
            libkf6dbusaddons-dev libkf6globalaccel-dev libepoxy-dev libvulkan-dev libdrm-dev \
            libwayland-dev pkg-config >/tmp/apt.log 2>&1 || { tail -5 /tmp/apt.log; exit 1; }
        cd '/work/way-vibrance-$VERSION'
        dpkg-buildpackage -us -uc -b >/tmp/dpkg.log 2>&1 || { tail -25 /tmp/dpkg.log; exit 1; }
    "
    mv -f "$STAGE"/way-vibrance_"${VERSION}${3}"_amd64.deb "$DIST"/ && echo "    $(ls "$DIST" | grep "deb" | tail -1)"
}

pkg_arch() {
    have_image archlinux:latest || { echo "==> SKIP arch (no image)"; return; }
    echo "==> PKG (Arch)"
    rm -rf "$STAGE/way-vibrance"; mkdir -p "$STAGE/way-vibrance"
    sed "s/@VERSION@/$VERSION/g" packaging/PKGBUILD > "$STAGE/way-vibrance/PKGBUILD"
    tar czf "$STAGE/way-vibrance/way-vibrance-$VERSION.tar.gz" -C "$STAGE" "way-vibrance-$VERSION"
    "$TOOL" run --rm -v "$STAGE:/work:rw" docker.io/library/archlinux:latest bash -c "
        pacman -Sy --noconfirm >/dev/null 2>&1
        pacman -S --noconfirm --needed base-devel cmake extra-cmake-modules kwin qt6-base \
            kconfig kcoreaddons kdbusaddons kglobalaccel vulkan-headers >/tmp/pacman.log 2>&1 || { tail -5 /tmp/pacman.log; exit 1; }
        # makepkg refuses to run as root: build as an unprivileged user.
        # chmod (not chown) keeps the artifacts owned by the bind-mount's
        # host user, so rootless podman never trips the /tmp sticky bit.
        useradd -m -u 1000 builder
        chmod -R a+rwX /work
        su builder -s /bin/bash -c 'cd /work/way-vibrance && makepkg -scf --noconfirm' >/tmp/makepkg.log 2>&1 || { tail -25 /tmp/makepkg.log; exit 1; }
        cp /work/way-vibrance/way-vibrance-$VERSION-1-x86_64.pkg.tar.zst /work/
    "
    mv -f "$STAGE"/way-vibrance-"$VERSION"-1-x86_64.pkg.tar.zst "$DIST"/ && echo "    $(ls "$DIST" | grep zst | tail -1)"
}

TARGETS=("$@"); ((${#TARGETS[@]})) || TARGETS=("${DEFAULT_TARGETS[@]}")
for t in "${TARGETS[@]}"; do
    case "$t" in
        fedora-*)  pkg_fedora "${t#fedora-}" ;;
        debian-13) pkg_deb debian:trixie-slim "Debian 13 stable" "~debian13" ;;
        debian-14) if have_image debian:forky 2>/dev/null; then img=debian:forky
                   elif have_image debian:testing; then img=debian:testing
                   else echo "==> SKIP debian-14 (no image)"; continue; fi
                   pkg_deb "$img" "Debian 14 testing" "~debian14" ;;
        ubuntu-*)  pkg_deb "ubuntu:${t#ubuntu-}" "Ubuntu ${t#ubuntu-}" "~ubuntu${t#ubuntu-}" ;;
        arch)      pkg_arch ;;
        *) echo "unknown target: $t (see the matrix in this file's header)" >&2; exit 1 ;;
    esac
done

echo
echo "==> Artifacts in dist/:"
ls -la "$DIST"
