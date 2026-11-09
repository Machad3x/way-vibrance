/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Shared between the tray's update checker (tray/main.cpp) and the
    versioned comparison test (tests/version_test.cpp), so the ordering the
    app ships and the ordering the test verifies are literally the same code.
*/

#pragma once

#include <QChar>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

namespace wvversion
{

/**
 * Compares dotted version strings, tolerating a leading 'v' and a
 * pre-release suffix ("1.2.3-rc1" compares as "1.2.3").
 *
 * Returns a negative value when @p lhs is older than @p rhs, zero when they
 * are equal under this ordering, and a positive value when newer. Missing
 * components count as zero, so "1.2" == "1.2.0".
 *
 * The tray only offers an update when GitHub's tag is strictly newer than
 * the running build (result > 0), so equality never triggers a false prompt.
 */
inline int compare(const QString &lhs, const QString &rhs)
{
    const auto normalize = [](QString s) {
        s = s.trimmed();
        if (s.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
            s.remove(0, 1);
        }
        const int dash = s.indexOf(QLatin1Char('-'));
        if (dash >= 0) {
            s.truncate(dash); // drop the pre-release suffix
        }
        return s.split(QLatin1Char('.'));
    };

    const QStringList a = normalize(lhs);
    const QStringList b = normalize(rhs);
    const int count = a.size() > b.size() ? a.size() : b.size();
    for (int i = 0; i < count; ++i) {
        // Missing components compare as zero, not as an empty string.
        const QString ca = i < a.size() ? a.at(i) : QStringLiteral("0");
        const QString cb = i < b.size() ? b.at(i) : QStringLiteral("0");
        bool okA = false;
        bool okB = false;
        const int av = ca.toInt(&okA);
        const int bv = cb.toInt(&okB);
        if (okA && okB) {
            if (av != bv) {
                return av < bv ? -1 : 1;
            }
        } else {
            // Non-numeric component: order lexically so odd tags still have a
            // stable, locale-independent meaning.
            const int cmp = ca.compare(cb);
            if (cmp != 0) {
                return cmp < 0 ? -1 : 1;
            }
        }
    }
    return 0;
}

} // namespace wvversion
