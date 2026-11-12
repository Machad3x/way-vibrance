/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Versioned guarantee for the tray's D-Bus owner verification: a service
// owner is trusted only when its PID matches the compositor's. Anything
// else (empty name, unknown bus name, non-compositor PID) must be rejected.

#include "dbus_security_p.h"

#include <QCoreApplication>
#include <QDBusConnection>

#include <cstdio>

static int s_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++s_failures;
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // Empty owner is never trusted.
    check(!ownerIsCompositor(QString()), "empty owner rejected");

    // An unknown / non-existent bus name resolves to PID 0 -> rejected.
    check(!ownerIsCompositor(QStringLiteral("org.wayvibrance.does.not.exist")),
          "unknown owner rejected");

    // org.freedesktop.DBus itself is not the compositor -> rejected (and in
    // this environment org.kde.KWin is not running, so kwinPid == 0 -> fail
    // closed).
    check(!ownerIsCompositor(QStringLiteral("org.freedesktop.DBus")),
          "non-compositor owner rejected");

    // pidOfBusName on an empty name is 0.
    check(pidOfBusName(QString()) == 0, "empty pid lookup is 0");

    // serviceOwnerOf on an unowned service is empty.
    check(serviceOwnerOf(QStringLiteral("org.wayvibrance.does.not.exist")).isEmpty(),
          "unowned service has no owner");

    if (s_failures == 0) {
        std::printf("dbus security guarantees: OK\n");
    }
    return s_failures == 0 ? 0 : 1;
}
