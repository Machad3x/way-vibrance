# SPDX-FileCopyrightText: 2026 machadex
# SPDX-License-Identifier: GPL-2.0-or-later
#
# RPM spec for Fedora (incl. install on Fedora Atomic/Kinoite via
# `rpm-ostree install way-vibrance`). Version is injected by
# scripts/package.sh from CMakeLists.txt.

%global debug_package %{nil}

Name:           way-vibrance
Version:        @VERSION@
Release:        1%{?dist}
Summary:        Digital vibrance, per-channel RGB gains and per-app profiles for KDE Plasma on Wayland

License:        GPL-2.0-or-later
URL:            https://github.com/Machad3x/way-vibrance
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  extra-cmake-modules
BuildRequires:  gcc-c++
BuildRequires:  kwin-devel
BuildRequires:  kf6-kconfig-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-kdbusaddons-devel
BuildRequires:  kf6-kglobalaccel-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  libepoxy-devel
BuildRequires:  vulkan-headers
BuildRequires:  libdrm-devel
BuildRequires:  wayland-devel

Requires:       plasma-workspace
Requires:       kf6-kglobalaccel

%description
System-wide vibrance/saturation boost, per-channel RGB gains and
per-application profiles for the KDE Plasma desktop on Wayland — the
counterpart of NVIDIA Digital Vibrance / vibrance-gui on platforms where
driver tools do not exist. Implemented as a KWin desktop effect with a
tray controller and CLI; everything runs in userspace over the session
D-Bus, with all state persisted to kwinrc.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release -DKDE_INSTALL_USE_QT_SYS_PATHS=ON
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%{_bindir}/wayvibrance
%{_bindir}/wayvibrance-tray
%{_libdir}/qt6/plugins/kwin/effects/plugins/wayvibrance.so
%{_datadir}/applications/org.kde.wayvibrance-tray.desktop

%changelog
* auto - machadex <machad3x@gmail.com>
- Built by scripts/package.sh; see https://github.com/Machad3x/way-vibrance/releases
