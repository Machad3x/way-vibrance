# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later

# KWin scans Qt plugin dirs for "kwin/effects/plugins" (see
# KWin::PluginEffectLoader in kwin/src/effect/effectloader.cpp).
configure_file(metadata.json ${CMAKE_CURRENT_BINARY_DIR}/metadata.json COPYONLY)

add_library(wayvibrance MODULE
    main.cpp
    vibranceeffect.cpp
    vibranceeffect.h
)

target_link_libraries(wayvibrance
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::DBus
        KF6::ConfigCore
        KWin::kwin
)

install(TARGETS wayvibrance
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins
)
