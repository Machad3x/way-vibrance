# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Versioned guarantees for the profile glob matcher (see file header).
add_executable(glob_match_test glob_match_test.cpp)
target_link_libraries(glob_match_test PRIVATE Qt6::Core)
target_include_directories(glob_match_test PRIVATE ${CMAKE_SOURCE_DIR}/effect ${CMAKE_SOURCE_DIR})
add_test(NAME glob_match_test COMMAND glob_match_test)

# Versioned guarantee for the circular slider reset button (shared header
# used by the tray window and the profile dialog).
add_executable(reset_button_test reset_button_test.cpp)
target_link_libraries(reset_button_test PRIVATE Qt6::Widgets)
target_include_directories(reset_button_test PRIVATE ${CMAKE_SOURCE_DIR}/tray)
add_test(NAME reset_button_test COMMAND reset_button_test)
set_tests_properties(reset_button_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Versioned guarantee for the palette-derived application stylesheet: theme
# awareness, contrast, no external resource loading.
add_executable(theme_style_test theme_style_test.cpp)
target_link_libraries(theme_style_test PRIVATE Qt6::Gui)
target_include_directories(theme_style_test PRIVATE ${CMAKE_SOURCE_DIR}/tray)
add_test(NAME theme_style_test COMMAND theme_style_test)

# Versioned guarantee for the segmented preset control: exclusive semantics,
# "no selection" for custom slider values and the QSS edge properties.
add_executable(preset_segment_test preset_segment_test.cpp)
target_link_libraries(preset_segment_test PRIVATE Qt6::Widgets)
target_include_directories(preset_segment_test PRIVATE ${CMAKE_SOURCE_DIR}/tray)
add_test(NAME preset_segment_test COMMAND preset_segment_test)
set_tests_properties(preset_segment_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Versioned guarantee for the on/off switch: QCheckBox semantics, whole-row
# hit target, keyboard activation, visible focus and palette/direction.
add_executable(switch_widget_test switch_widget_test.cpp)
target_link_libraries(switch_widget_test PRIVATE Qt6::Widgets)
target_include_directories(switch_widget_test PRIVATE ${CMAKE_SOURCE_DIR}/tray)
add_test(NAME switch_widget_test COMMAND switch_widget_test)
set_tests_properties(switch_widget_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Versioned guarantee for the update checker's ordering: a tag is only
# considered newer when it is strictly greater than the running build.
add_executable(version_test version_test.cpp)
target_link_libraries(version_test PRIVATE Qt6::Core)
target_include_directories(version_test PRIVATE ${CMAKE_SOURCE_DIR}/tray)
add_test(NAME version_test COMMAND version_test)

# Same guarantees under a 2x device pixel ratio: the focus ring is scaled by
# the painter and the hit/knob geometry must not assume logical pixels.
add_test(NAME switch_widget_test_hidpi COMMAND switch_widget_test)
set_tests_properties(switch_widget_test_hidpi PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2")

# Versioned guarantee for per-app profile (de)serialization round trip.
add_executable(config_roundtrip_test
    config_roundtrip_test.cpp
    ${CMAKE_SOURCE_DIR}/effect/vibranceeffect.cpp
)
target_link_libraries(config_roundtrip_test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::DBus
    Qt6::OpenGL
    KF6::ConfigCore
    KWin::kwin
)
target_include_directories(config_roundtrip_test PRIVATE
    ${CMAKE_SOURCE_DIR}/effect
    ${CMAKE_SOURCE_DIR}
)
set_target_properties(config_roundtrip_test PROPERTIES AUTOMOC ON)
add_test(NAME config_roundtrip_test COMMAND config_roundtrip_test)

# Versioned guarantee for the tray's D-Bus owner verification: only a
# compositor-owned service is trusted.
add_executable(dbus_security_test dbus_security_test.cpp)
target_link_libraries(dbus_security_test PRIVATE Qt6::Core Qt6::DBus)
target_include_directories(dbus_security_test PRIVATE ${CMAKE_SOURCE_DIR}/tray)
add_test(NAME dbus_security_test COMMAND dbus_security_test)
set_tests_properties(dbus_security_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
