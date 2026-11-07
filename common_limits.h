// SPDX-FileCopyrightText: 2026 machadex
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Common limits for Way Vibrance.

#pragma once

#include <cstdint>

// Constants for pattern matching and string lengths
constexpr int kMaxPatternLength = 128;
constexpr int kMaxNameLength = 64;
constexpr int kMaxOutputNameLength = 256;
constexpr int kMaxPrefixedPatternLength = kMaxPatternLength + 12;

// Constants for window identity and glob matching
constexpr int kMaxWindowIdentityLength = 1024;
constexpr int kGlobStepBudget = kMaxPatternLength * kMaxWindowIdentityLength + 4096;

// Constants for shader cache and app rules
constexpr int kMaxShaderVariants = 72;
constexpr float kShaderNearestMaxDelta = 0.02f;
constexpr int kMaxAppRules = 64;
constexpr int kMaxResolveWalks = 64 * 2;
constexpr int kResolveStepBudget = 1'000'000;