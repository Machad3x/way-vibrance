# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Versioned guarantees for the profile glob matcher (see file header).
add_executable(glob_match_test glob_match_test.cpp)
target_link_libraries(glob_match_test PRIVATE Qt6::Core)
target_include_directories(glob_match_test PRIVATE ${CMAKE_SOURCE_DIR}/effect)
add_test(NAME glob_match_test COMMAND glob_match_test)

# Versioned guarantee for the circular slider reset button (shared header
# used by the tray window and the profile dialog).
add_executable(reset_button_test reset_button_test.cpp)
target_link_libraries(reset_button_test PRIVATE Qt6::Widgets)
target_include_directories(reset_button_test PRIVATE ${CMAKE_SOURCE_DIR}/tray)
add_test(NAME reset_button_test COMMAND reset_button_test)
set_tests_properties(reset_button_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
