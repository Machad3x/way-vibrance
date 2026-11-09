# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(wayvibrance-tray
    main.cpp
)

# common_limits.h lives at the repo root, not beside these sources.
target_include_directories(wayvibrance-tray PRIVATE ${CMAKE_SOURCE_DIR})

# Single source of truth for the version shown in the About / update dialogs.
target_compile_definitions(wayvibrance-tray PRIVATE WAVERBANCE_VERSION="${PROJECT_VERSION}")

target_link_libraries(wayvibrance-tray
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Widgets
        Qt6::DBus
        KF6::CoreAddons
        KF6::DBusAddons
        KF6::GlobalAccel
        KF6::ConfigCore
)

install(TARGETS wayvibrance-tray DESTINATION ${KDE_INSTALL_BINDIR})

install(FILES org.kde.wayvibrance-tray.desktop DESTINATION ${KDE_INSTALL_APPDIR})
