/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Versioned guarantee for per-app profile (de)serialization: a rule survives
// a serializedRule() -> parseRule() round trip with its kind, pattern,
// vibrance, channel gains and display name intact.

#include "vibranceeffect.h"

#include <QCoreApplication>

#include <cstdio>

static int s_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++s_failures;
    }
}

static bool roundtrip(const KWin::VibranceEffect::AppRule &in)
{
    const QString ser = KWin::VibranceEffect::serializedRule(in);
    KWin::VibranceEffect::AppRule out{};
    if (!KWin::VibranceEffect::parseRule(ser, out)) {
        std::fprintf(stderr, "parseRule failed for: %s\n", qPrintable(ser));
        return false;
    }
    if (out.titleMatch != in.titleMatch) return false;
    if (out.pattern != in.pattern) return false;
    if (!qFuzzyCompare(out.vibrance, in.vibrance)) return false;
    if (!qFuzzyCompare(out.gains.x(), in.gains.x())) return false;
    if (!qFuzzyCompare(out.gains.y(), in.gains.y())) return false;
    if (!qFuzzyCompare(out.gains.z(), in.gains.z())) return false;
    if (out.displayName != in.displayName) return false;
    // Re-serialize and compare to lock the canonical form.
    if (KWin::VibranceEffect::serializedRule(out) != ser) return false;
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    {
        KWin::VibranceEffect::AppRule r;
        r.titleMatch = false;
        r.pattern = QStringLiteral("com.example.app");
        r.vibrance = 1.4;
        r.gains = QVector3D(1.0f, 1.0f, 1.0f);
        r.displayName = QStringLiteral("Example");
        check(roundtrip(r), "app rule with name");
    }
    {
        KWin::VibranceEffect::AppRule r;
        r.titleMatch = true;
        r.pattern = QStringLiteral("*game*");
        r.vibrance = 0.8;
        r.gains = QVector3D(1.2f, 0.9f, 1.1f);
        check(roundtrip(r), "title rule with gains");
    }
    {
        KWin::VibranceEffect::AppRule r;
        r.pattern = QStringLiteral("firefox");
        r.vibrance = 1.0;
        r.gains = QVector3D(1.0f, 1.0f, 1.0f);
        check(roundtrip(r), "neutral gains, no name");
    }
    {
        // Gains + name both present: name lands in 4th slot.
        KWin::VibranceEffect::AppRule r;
        r.pattern = QStringLiteral("steam");
        r.vibrance = 2.0;
        r.gains = QVector3D(0.5f, 0.5f, 0.5f);
        r.displayName = QStringLiteral("Steam");
        check(roundtrip(r), "gains and name");
    }

    // Invalid inputs must be rejected.
    {
        KWin::VibranceEffect::AppRule out{};
        check(!KWin::VibranceEffect::parseRule(QStringLiteral(""), out),
              "empty rejected");
        check(!KWin::VibranceEffect::parseRule(QStringLiteral("app:foo"), out),
              "missing vibrance rejected");
        check(!KWin::VibranceEffect::parseRule(QStringLiteral("app:foo|abc"), out),
              "non-numeric vibrance rejected");
        check(!KWin::VibranceEffect::parseRule(QStringLiteral("app:foo|1.0|1,2"), out),
              "partial gains rejected");
        check(!KWin::VibranceEffect::parseRule(QStringLiteral("app:foo|1.0|1,2,3,4"), out),
              "too many gain fields rejected");
        check(!KWin::VibranceEffect::parseRule(QStringLiteral("app: |1.0"), out),
              "empty pattern rejected");
    }

    if (s_failures == 0) {
        std::printf("config round-trip guarantees: OK\n");
    }
    return s_failures == 0 ? 0 : 1;
}
