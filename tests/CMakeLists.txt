# SPDX-FileCopyrightText: 2026 machadex
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Versioned guarantees for the profile glob matcher (see file header).
add_executable(glob_match_test glob_match_test.cpp)
target_link_libraries(glob_match_test PRIVATE Qt6::Core)
target_include_directories(glob_match_test PRIVATE ${CMAKE_SOURCE_DIR}/effect)
add_test(NAME glob_match_test COMMAND glob_match_test)
