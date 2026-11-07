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
    wayvibrance.qrc
)

# common_limits.h lives at the repo root, not beside these sources.
target_include_directories(wayvibrance PRIVATE ${CMAKE_SOURCE_DIR})

# KWin's factory macros produce Q_OBJECT code that AUTOMOC does not know by
# name; teach it so main.moc is generated and the (misleading) "no Q_OBJECT
# macro" warning disappears without dropping the moc include.
set_target_properties(wayvibrance PROPERTIES
    AUTOMOC_MACRO_NAMES "Q_OBJECT;Q_GADGET;Q_NAMESPACE;Q_NAMESPACE_EXPORT;KWIN_EFFECT_FACTORY;KWIN_EFFECT_FACTORY_SUPPORTED"
)

target_link_libraries(wayvibrance
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::DBus
        Qt6::OpenGL
        KF6::ConfigCore
        KWin::kwin
)

install(TARGETS wayvibrance
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins
)
