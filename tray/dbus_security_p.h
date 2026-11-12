/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Security-critical D-Bus owner verification helpers for the tray. Extracted
// so they can be unit-tested: the tray trusts org.wayvibrance only when its
// owning PID matches the compositor's (org.kde.KWin). A session peer that
// grabs the well-known name before the effect registers must not pass.

#pragma once

#include <QDBusConnection>
#include <QDBusMessage>

static constexpr int kBusIdentityTimeoutMs = 1000;

/** Unique-name owner of a well-known service, or empty when unowned. */
inline QString serviceOwnerOf(const QString &service, int timeoutMs = kBusIdentityTimeoutMs)
{
    const QDBusMessage reply = QDBusConnection::sessionBus().call(
        QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                       QStringLiteral("/org/freedesktop/DBus"),
                                       QStringLiteral("org.freedesktop.DBus"),
                                       QStringLiteral("GetNameOwner"))
            << service,
        QDBus::Block, timeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return QString();
    }
    return reply.arguments().first().toString();
}

/** PID of a connection name (well-known or unique); 0 on error. */
inline uint pidOfBusName(const QString &busName, int timeoutMs = kBusIdentityTimeoutMs)
{
    if (busName.isEmpty()) {
        return 0;
    }
    const QDBusMessage reply = QDBusConnection::sessionBus().call(
        QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                       QStringLiteral("/org/freedesktop/DBus"),
                                       QStringLiteral("org.freedesktop.DBus"),
                                       QStringLiteral("GetConnectionUnixProcessID"))
            << busName,
        QDBus::Block, timeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return 0;
    }
    return reply.arguments().first().toUInt();
}

/**
 * True when the connection owning org.wayvibrance is the compositor
 * process itself. Only org.kde.KWin is trusted as the anchor.
 */
inline bool ownerIsCompositor(const QString &owner)
{
    if (owner.isEmpty()) {
        return false;
    }
    const uint pid = pidOfBusName(owner);
    if (pid == 0) {
        return false;
    }
    const uint kwinPid = pidOfBusName(QStringLiteral("org.kde.KWin"));
    return kwinPid != 0 && pid == kwinPid;
}
