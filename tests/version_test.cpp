/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Versioned guarantee for the update checker's ordering: the tray only
// offers an update when GitHub's tag is strictly newer than the build.

#include "version_p.h"

#include <QCoreApplication>

#include <cstdio>

static int s_failures = 0;

static void check(const QString &lhs, const QString &rhs, int expectedSign, const char *what)
{
    const int actual = wvversion::compare(lhs, rhs);
    const int sign = actual < 0 ? -1 : (actual > 0 ? 1 : 0);
    if (sign != expectedSign) {
        std::fprintf(stderr, "FAIL %s: compare(%s, %s) -> %d, expected sign %d\n", what,
                     qPrintable(lhs), qPrintable(rhs), actual, expectedSign);
        ++s_failures;
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    check(QStringLiteral("0.6.5"), QStringLiteral("0.6.5"), 0, "equal");
    check(QStringLiteral("v0.6.5"), QStringLiteral("0.6.5"), 0, "leading v on lhs");
    check(QStringLiteral("0.6.5"), QStringLiteral("v0.6.5"), 0, "leading v on rhs");
    check(QStringLiteral("0.6.6"), QStringLiteral("0.6.5"), 1, "patch newer");
    check(QStringLiteral("0.6.4"), QStringLiteral("0.6.5"), -1, "patch older");
    check(QStringLiteral("0.7.0"), QStringLiteral("0.6.9"), 1, "minor newer");
    check(QStringLiteral("1.0.0"), QStringLiteral("0.99.99"), 1, "major newer");
    check(QStringLiteral("0.6"), QStringLiteral("0.6.0"), 0, "missing component is zero");
    check(QStringLiteral("0.6"), QStringLiteral("0.6.1"), -1, "missing component older");
    check(QStringLiteral("v1.2.3"), QStringLiteral("v1.2.3-rc1"), 0, "pre-release compares as base");
    check(QStringLiteral("10.0.0"), QStringLiteral("9.9.9"), 1, "numeric, not lexical");
    check(QStringLiteral("1.0-beta"), QStringLiteral("1.0-alpha"), 0,
          "pre-release suffix is ignored");
    check(QStringLiteral("1.0beta"), QStringLiteral("1.0alpha"), 1, "non-numeric component");

    if (s_failures == 0) {
        std::printf("version comparison guarantees: OK\n");
    }
    return s_failures == 0 ? 0 : 1;
}
