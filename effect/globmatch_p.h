/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Shared between the effect (effect/vibranceeffect.cpp) and the versioned
    matcher test (tests/glob_match_test.cpp) so the fuzz-verified behavior
    and the shipped behavior are, literally, the same code.
*/

#pragma once

#include <QChar>
#include <QLatin1Char>
#include <QString>

// Input ceilings enforced at every D-Bus/config entry point (see
// docs/FEATURES.md "Per-app profiles — Limits").
constexpr int kMaxPatternLength = 128;
constexpr int kMaxWindowIdentityLength = 1024;

// Hard step budget for a single globMatch() call, sized strictly ABOVE the
// worst case of the greedy matcher within the caps above: each iteration
// either advances the text or rewinds to a star (which then advances the
// text), so total steps are bounded by pattern.size() * text.size(); the
// product plus slack is therefore unreachable by legitimate input and the
// budget cannot alter matching behavior today. It becomes an enforced
// ceiling the moment any cap above is raised, so a hostile pattern can
// never turn into unbounded work on the paint path.
constexpr int kGlobStepBudget = kMaxPatternLength * kMaxWindowIdentityLength + 4096;

// Aggregate budget for one resolveWindow() pass (up to 64 rules, app rules
// scanning two identities). Realistic rule sets consume a few thousand
// steps end-to-end; the aggregate caps adversarial all-worst-case sets at
// ~1ms per identity CHANGE (never per frame — unchanged identities exit
// through the stamp/compare fast path long before matching).
constexpr int kResolveStepBudget = 1'000'000;

/**
 * Matches a user glob against a string, anchored and case-insensitive:
 * '*' spans any run, '?' consumes exactly one character, everything else
 * is literal.
 *
 * Iterative wildcards matcher with a single rewind point: worst-case work
 * is bounded by glob.size() * text.size() with a tiny constant and the
 * explicit budget ceilings passed in; no regex engine, hence no
 * backtracking blow-ups on the paint path.
 *
 * @p stepBudget (when not null) is decremented by the steps consumed and
 * matching stops (no-match) once it is exhausted — callers evaluating many
 * patterns share one budget through it. The per-call kGlobStepBudget always
 * applies on top.
 */
inline bool globMatch(const QString &glob, const QString &text, int *stepBudget = nullptr)
{
    int g = 0;
    int t = 0;
    int starAt = -1;
    int resumeAt = -1;
    int steps = 0;
    const auto sameCase = [](QChar a, QChar b) {
        return a == b || a.toLower() == b.toLower() || a.toUpper() == b.toUpper();
    };
    while (t < text.size()) {
        if (++steps > kGlobStepBudget || (stepBudget && --(*stepBudget) < 0)) {
            return false; // budgeted: unreachable within shipped caps (see constants)
        }
        if (g < glob.size() && glob.at(g) == QLatin1Char('?')) {
            ++g;
            ++t;
        } else if (g < glob.size() && glob.at(g) != QLatin1Char('*') && sameCase(glob.at(g), text.at(t))) {
            ++g;
            ++t;
        } else if (g < glob.size() && glob.at(g) == QLatin1Char('*')) {
            starAt = g++; // consume the star; let the rewind below feed it chars
            resumeAt = t;
        } else if (starAt >= 0) {
            g = starAt + 1;
            t = ++resumeAt;
        } else {
            return false;
        }
    }
    while (g < glob.size() && glob.at(g) == QLatin1Char('*')) {
        ++g;
    }
    return g == glob.size();
}
