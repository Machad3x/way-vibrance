# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(wayvibrance-tray
    main.cpp
)

target_link_libraries(wayvibrance-tray
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::DBus
        KF6::CoreAddons
        KF6::DBusAddons
        KF6::GlobalAccel
)

install(TARGETS wayvibrance-tray DESTINATION ${KDE_INSTALL_BINDIR})

install(FILES wayvibrance.desktop DESTINATION ${KDE_INSTALL_APPDIR})
