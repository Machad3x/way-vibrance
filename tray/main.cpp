/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Way Vibrance tray controller: the Linux/Wayland (KDE) counterpart of
// vibrance-gui. Talks to the "wayvibrance" KWin effect over D-Bus for
// flicker-free live adjustments.

#include "resetbutton_p.h"
#include "segmented_p.h"
#include "style_p.h"
#include "switch_p.h"
#include "version_p.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPointer>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QGroupBox>
#include <QAction>
#include <QIcon>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

#include <KAboutData>
#include <KConfigGroup>
#include <KDBusService>
#include "common_limits.h"
#include <KGlobalAccel>
#include <KGlobalShortcutInfo>
#include <KSharedConfig>

static constexpr auto s_effectId = "wayvibrance";

// Effect-services probe order for ensureEffectLoaded (see tryLoadEffectVia).
static constexpr const char *kServices[] = {"org.kde.KWin", "org.kde.kwin.Effects"};
static constexpr auto s_vibranceService = "org.wayvibrance";
static constexpr auto s_vibrancePath = "/Vibrance";
static constexpr auto s_vibranceInterface = "org.wayvibrance";
static constexpr auto s_toggleActionId = "toggle-vibrance";
static constexpr int kMinPercent = 0;
static constexpr int kMaxPercent = 200;
static constexpr int kRgbMinPercent = 0;
static constexpr int kRgbMaxPercent = 200;

// Same ceilings the effect enforces on D-Bus input — the tray mirrors them
// so a hostile emitter cannot inflate the UI list or the parsed fields.
static constexpr int kRefreshTimeoutMs = 3000;

/** Finite + within the effect's accepted range; the UI speaks percents. */
static bool saneFactor(const QVariant &v, double &out)
{
    if (v.metaType().id() != QMetaType::Double || !std::isfinite(v.toDouble())) {
        return false;
    }
    out = qBound(0.0, v.toDouble(), 2.0);
    return true;
}

/** Same ceilings applied to a profile list from any source (signal or
 *  method reply): count, per-entry pattern length, per-entry total size. */
static QStringList sanitizeRuleList(const QStringList &in)
{
    QStringList rules = in;
    if (rules.size() > kMaxAppRules) {
        rules = rules.mid(0, kMaxAppRules);
    }
    rules.removeIf([](const QString &e) {
        const int sep = e.indexOf(QLatin1Char('|'));
        const int patternLen = sep < 0 ? e.size() : sep;
        return e.isEmpty() || patternLen > kMaxPrefixedPatternLength || e.size() > 512;
    });
    return rules;
}

// Identity probes are synchronous by nature (the PID must be known before the
// interface is trusted), but they are bounded: a wedged bus must not freeze
// the tray for Qt's 25-second default per call.
static constexpr int kBusIdentityTimeoutMs = 1000;

/** Unique-name owner of a well-known service, or empty when unowned. */
static QString serviceOwnerOf(const QString &service, int timeoutMs = kBusIdentityTimeoutMs)
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
static uint pidOfBusName(const QString &busName, int timeoutMs = kBusIdentityTimeoutMs)
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
 * process itself. Checking the well-known name alone is not enough: a
 * session peer can grab it before the effect registers. The authoritative
 * identity is the PID, compared with the owner of org.kde.KWin.
 *
 * Only org.kde.KWin is trusted as the anchor. The legacy
 * org.kde.kwin.Effects name is deliberately NOT accepted: every KWin 6
 * release owns org.kde.KWin unconditionally, and the legacy name is usually
 * free, so accepting it would let a malicious peer own both names and pass
 * the PID comparison. There is no /proc/<pid>/comm fallback either:
 * prctl(PR_SET_NAME) forges process names trivially.
 */
static bool ownerIsCompositor(const QString &owner)
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

/**
 * Bridges the UI to the effect over D-Bus, coalescing slider movements so
 * the compositor is not hammered with repaints on every event.
 */
class VibranceController : public QObject
{
    Q_OBJECT

public:
    explicit VibranceController(QObject *parent = nullptr)
        : QObject(parent)
        , m_watcher(QString::fromLatin1(s_vibranceService),
                    QDBusConnection::sessionBus(),
                    QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration)
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(40);
        connect(&m_timer, &QTimer::timeout, this, &VibranceController::flush);

        m_rgbTimer.setSingleShot(true);
        m_rgbTimer.setInterval(40);
        connect(&m_rgbTimer, &QTimer::timeout, this, &VibranceController::flushRGB);

        m_toggleAction = new QAction(QObject::tr("Toggle Way Vibrance"), this);
        m_toggleAction->setObjectName(QString::fromLatin1(s_toggleActionId));
        connect(m_toggleAction, &QAction::triggered, this, &VibranceController::toggleVibrance);

        // The file is the source of truth even before we talk to the daemon.
        m_toggleShortcut = storedToggleShortcut();

        connect(&m_watcher, &QDBusServiceWatcher::serviceRegistered,
                this, &VibranceController::onServiceRegistered);
        connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered,
                this, &VibranceController::onServiceUnregistered);

        // Well-known names are spoofable by any session peer, so signals are
        // trusted by UNIQUE sender identity: serviceOwnerChanged keeps the
        // current owner cached and onSignal() drops anything else. The initial
        // owner is resolved (with a bounded timeout) inside attachInterface().
        connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &name, const QString &, const QString &newOwner) {
                    if (name != QLatin1String(s_vibranceService)) {
                        return;
                    }
                    if (newOwner == m_serviceOwner) {
                        return; // same connection still owns the name
                    }
                    // A direct owner swap must not inherit trust: drop the
                    // existing state first, then let attachInterface()
                    // re-resolve and re-verify the new owner (compositor-PID
                    // check included) before any signal reaches the UI.
                    // Delay re-attach by 100ms to tolerate KWin restarts and
                    // avoid accepting a transient owner during service handover.
                    detachInterface();
                    m_serviceOwner = newOwner;
                    if (!newOwner.isEmpty()) {
                        QTimer::singleShot(100, this, [this] { attachInterface(); });
                    }
                });

        // The effect may already be running (loaded by a previous session's
        // configuration); attachInterface() resolves the owner with a bounded
        // timeout and returns silently when the name is not currently owned.
        attachInterface();
    }

    bool effectAvailable() const { return m_attached; }

    /** The QAction backing the configurable global hotkey (toggle on/off). */
    QAction *toggleAction() const { return m_toggleAction; }

    /** Loads the effect into KWin if it is not already running. */
    void ensureEffectLoaded()
    {
        // KWin 6.8+ hosts the /Effects object under the org.kde.KWin service;
        // older setups also exported a dedicated org.kde.kwin.Effects service.
        // Try both, same interface.
        tryLoadEffectVia(QLatin1String(kServices[0]), 1);
    }

private:
    /**
     * Async probe chain over the KWin effect services: isEffectLoaded with
     * a bounded timeout (a wedged compositor cannot freeze the tray), then
     * fire-and-forget loadEffect. If the effect comes up it registers
     * org.wayvibrance and the service watcher attaches to it.
     */
    void tryLoadEffectVia(const QString &service, int fallbacksLeft)
    {
        QDBusMessage probe = QDBusMessage::createMethodCall(
            service, QStringLiteral("/Effects"),
            QStringLiteral("org.kde.kwin.Effects"),
            QStringLiteral("isEffectLoaded"));
        probe.setArguments({QString::fromLatin1(s_effectId)});
        QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(probe, kRefreshTimeoutMs);
        auto *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, service, fallbacksLeft](QDBusPendingCallWatcher *w) {
                    const bool isError = w->isError();
                    const bool known = !isError && w->reply().arguments().value(0).toBool();
                    w->deleteLater();
                    if (known) {
                        return; // already running: the watcher will have attached
                    }
                    if (isError && fallbacksLeft > 0) {
                        tryLoadEffectVia(QLatin1String(kServices[1]), fallbacksLeft - 1);
                        return;
                    }
                    // Plain async message: constructing a QDBusInterface here
                    // would introspect the service synchronously (Qt's 25 s
                    // default timeout), defeating the bounded probe above.
                    QDBusMessage load = QDBusMessage::createMethodCall(
                        service, QStringLiteral("/Effects"),
                        QStringLiteral("org.kde.kwin.Effects"),
                        QStringLiteral("loadEffect"));
                    load.setArguments({QString::fromLatin1(s_effectId)});
                    QDBusConnection::sessionBus().asyncCall(load, kRefreshTimeoutMs);
                });
    }

public:
    void requestVibrancePercent(int percent)
    {
        m_pendingPercent = percent;
        m_timer.start();
    }

    void requestVibranceEnabled(bool enabled)
    {
        send(QStringLiteral("setEnabled"), {enabled});
    }

    void requestPrimaryOnly(bool primaryOnly)
    {
        if (m_attached) {
            send(QStringLiteral("setPrimaryOnly"), {primaryOnly});
            QTimer::singleShot(160, this, &VibranceController::refreshState);
        }
    }

    /** Async screen list pull; result (sanitized) arrives via
     *  screenListFromEffect(name, entries). A hung compositor can no
     *  longer block whoever opened the monitor dropdown. */
    void requestScreenList(const QString &currentName)
    {
        if (!m_attached) {
            Q_EMIT screenListFromEffect(currentName, {});
            return;
        }
        QDBusMessage msg = QDBusMessage::createMethodCall(
            m_serviceOwner,
            QString::fromLatin1(s_vibrancePath),
            QString::fromLatin1(s_vibranceInterface),
            QStringLiteral("screenList"));
        QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(msg, kRefreshTimeoutMs);
        auto *watcher = new QDBusPendingCallWatcher(call, this);
        watcher->setProperty("currentName", currentName);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this](QDBusPendingCallWatcher *w) {
                    const QString name = w->property("currentName").toString();
                    QStringList entries = w->isError()
                        ? QStringList()
                        : w->reply().arguments().value(0).toStringList();
                    w->deleteLater();
                    if (entries.size() > 64) { // hostile-emitter ceiling
                        entries = entries.mid(0, 64);
                    }
                    entries.removeIf([](const QString &e) { return e.size() > 512; });
                    Q_EMIT screenListFromEffect(name, entries);
                });
    }

    /** Empty name selects "automatic" (first output) on the effect side. */
    void requestSetPrimaryOutput(const QString &name)
    {
        if (m_attached) {
            send(QStringLiteral("setPrimaryOutputName"), {name});
            QTimer::singleShot(160, this, &VibranceController::refreshState);
        }
    }

    bool primaryOutputSupported() const { return m_primaryOutputSupported; }

    /**
     * Pulls the full current state from the effect and re-emits it as if it
     * had changed remotely, so every slider/checkbox is truthful even if a
     * D-Bus signal was ever missed (and on startup, before any change).
     */
    /**
     * Pull the full effect state asynchronously: eight state queries (plus
     * one conditional capability Introspect) with a bounded timeout never
     * block the GUI thread — a busy or hung KWin can
     * no longer freeze the tray (the old sequential blocking form could
     * stall it for the bus timeout per call). Replies are validated exactly
     * like signal arguments; a monotonic sequence stamp keeps stale rounds
     * (and anything in flight across a detach) from touching the UI.
     */
    void refreshState()
    {
        if (!m_attached) {
            return;
        }
        const quint64 seq = ++m_refreshSeq;
        m_gainMask = 0;
        const auto issue = [this, seq](const QString &method) {
            QDBusMessage msg = QDBusMessage::createMethodCall(
                m_serviceOwner,
                QString::fromLatin1(s_vibrancePath),
                QString::fromLatin1(s_vibranceInterface),
                method);
            const QDBusPendingCall call =
                QDBusConnection::sessionBus().asyncCall(msg, kRefreshTimeoutMs);
            auto *watcher = new QDBusPendingCallWatcher(call, this);
            watcher->setProperty("kind", method);
            watcher->setProperty("seq", seq);
            connect(watcher, &QDBusPendingCallWatcher::finished,
                    this, &VibranceController::onRefreshReply);
        };
        static const char *const kQueries[] = {
            "vibrance", "enabled", "primaryOnly", "primaryOutputName",
            "redGain", "greenGain", "blueGain", "appRules",
        };
        for (const char *query : kQueries) {
            issue(QLatin1String(query));
        }
    }


    void requestRGBGains(double red, double green, double blue)
    {
        m_pendingRed = red;
        m_pendingGreen = green;
        m_pendingBlue = blue;
        m_rgbPending = true;
        m_rgbTimer.start();
    }

    void requestSetAppRuleFull(const QString &prefixedPattern, double vibrance,
                               double red, double green, double blue)
    {
        if (!m_attached) {
            return;
        }
        if (m_profilesFull) {
            send(QStringLiteral("setAppRuleFull"),
                 {prefixedPattern, vibrance, red, green, blue});
        } else {
            // Older effect: vibrance-only fallback (gains are not offered in
            // the dialog in this mode anyway).
            send(QStringLiteral("setAppRule"), {prefixedPattern, vibrance});
        }
        QTimer::singleShot(120, this, &VibranceController::refreshState);
    }

    /** Cosmetic list label; matching keeps using the pattern. */
    void requestSetAppRuleName(const QString &prefixedPattern, const QString &name)
    {
        if (m_attached && m_profilesFull) {
            send(QStringLiteral("setAppRuleName"), {prefixedPattern, name});
            QTimer::singleShot(120, this, &VibranceController::refreshState);
        }
    }

    void requestRemoveAppRule(const QString &prefixedPattern)
    {
        if (m_attached) {
            send(QStringLiteral("removeAppRule"), {prefixedPattern});
            QTimer::singleShot(120, this, &VibranceController::refreshState);
        }
    }

    /** Resource class (Wayland app_id / X11 WM_CLASS) of the last focused app. */
    /** Async "last focused app id" pull for the dialog pre-fill button;
     *  cb runs on the reply (empty on error/timeout). A second click while
     *  pending is ignored instead of stacking dialogs. */
    void requestLastFocusedAppId(std::function<void(const QString &)> cb)
    {
        if (m_lastFocusedPending) {
            return; // a reply for the earlier click is already coming; no dialog stacking
        }
        if (!m_attached) {
            cb(QString()); // no effect: open the dialog with an empty pre-fill (as before)
            return;
        }
        m_lastFocusedPending = true;
        m_lastFocusedCb = std::move(cb);
        // Replies already in flight from an earlier attachment (or an earlier
        // click after a detach) must not consume this click's callback.
        const quint64 seq = ++m_lastFocusedSeq;
        QDBusMessage msg = QDBusMessage::createMethodCall(
            m_serviceOwner,
            QString::fromLatin1(s_vibrancePath),
            QString::fromLatin1(s_vibranceInterface),
            QStringLiteral("lastFocusedAppId"));
        QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(msg, kRefreshTimeoutMs);
        auto *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, seq](QDBusPendingCallWatcher *w) {
                    const QString id = w->isError()
                        ? QString()
                        : w->reply().arguments().value(0).toString().left(1024);
                    w->deleteLater();
                    if (seq != m_lastFocusedSeq) {
                        return; // detached or superseded meanwhile
                    }
                    m_lastFocusedPending = false;
                    auto cb = std::exchange(m_lastFocusedCb, nullptr);
                    if (cb) {
                        cb(id); // bounded even if the effect were older/hostile
                    }
                });
    }


    bool profilesSupported() const { return m_profilesSupported; }
    /** True when the connected effect understands gains + display names. */
    bool profilesFullApi() const { return m_profilesSupported && m_profilesFull; }

    /**
     * (Re)registers the toggle hotkey with KGlobalAccel and persists the
     * choice in our own config (wayvibrancerc). The application's own
     * component cannot be read back reliably from the daemon at startup
     * (the daemon introspects us while we are still registering), so
     * wayvibrancerc is the source of truth and we re-register every
     * session. An empty sequence unbinds the hotkey — using
     * removeAllShortcuts(), because setGlobalShortcut() with an empty
     * list is a no-op.
     */
    bool registerToggleShortcut(const QKeySequence &sequence)
    {
        m_toggleShortcut = sequence;
        bool ok = true;
        if (sequence.isEmpty()) {
            KGlobalAccel::self()->removeAllShortcuts(m_toggleAction);
        } else {
            ok = KGlobalAccel::self()->setGlobalShortcut(m_toggleAction, sequence);
        }
        saveToggleShortcut(sequence);
        return ok;
    }

    QKeySequence toggleShortcut() const { return m_toggleShortcut; }
    bool shortcutsReady() const { return m_shortcutsReady; }
    bool externalChangesAccepted() const { return m_externalsTrusted; }

    /**
     * Runs from the event loop shortly after startup: wipes any state the
     * daemon kept for our action (older builds registered Meta+V and the
     * daemon pushes that stale binding back to us as soon as we listen!)
     * and re-applies exactly what wayvibrancerc says. External edits (e.g.
     * System Settings) are only mirrored into our config after a short
     * grace period, so the stale startup burst cannot poison the file.
     */
    void applyStartupShortcut()
    {
        KGlobalAccel::self()->removeAllShortcuts(m_toggleAction);
        if (!m_toggleShortcut.isEmpty()) {
            KGlobalAccel::self()->setGlobalShortcut(m_toggleAction, m_toggleShortcut);
        }
        m_shortcutsReady = true;
        QTimer::singleShot(3000, this, [this] { m_externalsTrusted = true; });
    }

    /** Our own, reliable persistence for the captured hotkey. */
    static QKeySequence storedToggleShortcut()
    {
        auto config = KSharedConfig::openConfig(QStringLiteral("wayvibrancerc"));
        const KConfigGroup group(config, QStringLiteral("Shortcuts"));
        // Key name bumped from "ToggleShortcut": the old key could hold a
        // Meta+V value poisoned by the startup race fixed here.
        return QKeySequence(group.readEntry("GlobalToggleShortcut", QString()));
    }

    static void saveToggleShortcut(const QKeySequence &sequence)
    {
        auto config = KSharedConfig::openConfig(QStringLiteral("wayvibrancerc"));
        KConfigGroup group(config, QStringLiteral("Shortcuts"));
        group.writeEntry("GlobalToggleShortcut", sequence.toString());
        // Drop the poisoned legacy key if present.
        group.deleteEntry("ToggleShortcut");
        config->sync();
    }

Q_SIGNALS:
    void availableChanged(bool available);
    /** Emitted when the value changed remotely (effect/CLI), not by this UI. */
    void vibranceFromEffect(double value);
    void enabledFromEffect(bool enabled);
    void rgbGainsFromEffect(double red, double green, double blue);
    void primaryOnlyFromEffect(bool primaryOnly);
    /** Monitor-picker capability + current selection from the effect. */
    void primaryOutputStateFromEffect(bool supported, const QString &name);
    void screenListFromEffect(const QString &name, const QStringList &entries);
    void appRulesFromEffect(const QStringList &rules);
    /** False while talking to an effect build that has no per-app profiles. */
    void profilesSupportChanged(bool supported);

private Q_SLOTS:
    /**
     * Single catch-all D-Bus signal receiver (see attachInterface): Qt's
     * typed string-based bus.connect() silently receives nothing with this
     * Qt/dbus-broker combination, while the generic QDBusMessage form does
     * deliver. So we subscribe once and fan out here.
     */
    void onSignal(const QDBusMessage &msg)
    {
        if (msg.interface() != QLatin1String(s_vibranceInterface)
            || msg.path() != QLatin1String(s_vibrancePath)) {
            return;
        }
        // Only the connection that currently OWNS the well-known name is
        // trusted (that owner is kept fresh via serviceOwnerChanged).
        if (m_serviceOwner.isEmpty() || msg.service() != m_serviceOwner || !m_attached) {
            return;
        }
        const QList<QVariant> args = msg.arguments();
        const QString member = msg.member();
        if (member == QLatin1String("vibranceChanged") && args.size() == 1) {
            double v = 0.0;
            if (saneFactor(args.at(0), v)) {
                onVibranceChanged(v);
            }
        } else if (member == QLatin1String("enabledChanged") && args.size() == 1) {
            if (args.at(0).metaType().id() == QMetaType::Bool) {
                onEnabledChanged(args.at(0).toBool());
            }
        } else if (member == QLatin1String("rgbGainsChanged") && args.size() == 3) {
            double r = 0.0, g = 0.0, b = 0.0;
            if (saneFactor(args.at(0), r) && saneFactor(args.at(1), g) && saneFactor(args.at(2), b)) {
                onRGBGainsChanged(r, g, b);
            }
        } else if (member == QLatin1String("primaryOnlyChanged") && args.size() == 1) {
            if (args.at(0).metaType().id() == QMetaType::Bool) {
                onPrimaryOnlyChanged(args.at(0).toBool());
            }
        } else if (member == QLatin1String("appRulesChanged") && args.size() == 1) {
            onAppRulesChanged(sanitizeRuleList(args.at(0).toStringList()));
        }
    }

    void onVibranceChanged(double value)
    {
        // Keep the send-dedupe cache aligned with the effect's authoritative
        // value. Without this, after a CLI/remote change, dragging the slider
        // back to the last value we sent would be treated as a no-op and the
        // UI/effect would stay out of sync. A queued local send wins instead.
        if (m_pendingPercent < 0) {
            m_lastSent = value;
        }
        Q_EMIT vibranceFromEffect(value);
    }

    void onAppRulesChanged(const QStringList &rules)
    {
        Q_EMIT appRulesFromEffect(rules);
    }

    void onEnabledChanged(bool enabled)
    {
        Q_EMIT enabledFromEffect(enabled);
    }

    void onRGBGainsChanged(double red, double green, double blue)
    {
        Q_EMIT rgbGainsFromEffect(red, green, blue);
    }

    void onPrimaryOnlyChanged(bool primaryOnly)
    {
        Q_EMIT primaryOnlyFromEffect(primaryOnly);
    }

    /** One completed refresh query (see refreshState). */
    void onRefreshReply(QDBusPendingCallWatcher *watcher)
    {
        const quint64 seq = watcher->property("seq").toULongLong();
        const QString kind = watcher->property("kind").toString();
        const bool isError = watcher->isError();
        const QDBusMessage reply = watcher->reply(); // is an error message when isError
        watcher->deleteLater();
        if (seq != m_refreshSeq || !m_attached) {
            return; // stale round (or detached meanwhile)
        }
        const QList<QVariant> args = reply.arguments();
        double factor = 1.0;
        if (kind == QLatin1String("vibrance")) {
            if (!isError && args.size() == 1 && saneFactor(args.at(0), factor)) {
                onVibranceChanged(factor);
            }
        } else if (kind == QLatin1String("enabled")) {
            if (!isError && args.size() == 1 && args.at(0).metaType().id() == QMetaType::Bool) {
                onEnabledChanged(args.at(0).toBool());
            }
        } else if (kind == QLatin1String("primaryOnly")) {
            if (!isError && args.size() == 1 && args.at(0).metaType().id() == QMetaType::Bool) {
                onPrimaryOnlyChanged(args.at(0).toBool());
            }
        } else if (kind == QLatin1String("primaryOutputName")) {
            // Monitor-picking support: older effects answer UnknownMethod; we
            // then hide the dropdown instead of pushing into a void.
            m_primaryOutputSupported = !isError;
            Q_EMIT primaryOutputStateFromEffect(m_primaryOutputSupported,
                                                m_primaryOutputSupported
                                                    ? args.value(0).toString().left(kMaxOutputNameLength)
                                                    : QString());
        } else if (kind == QLatin1String("redGain") || kind == QLatin1String("greenGain")
                   || kind == QLatin1String("blueGain")) {
            const int channel = kind.at(0) == QLatin1Char('r') ? 0 : (kind.at(0) == QLatin1Char('g') ? 1 : 2);
            const int bit = 1 << channel;
            if (isError || args.size() != 1 || !saneFactor(args.at(0), factor)) {
                m_gainMask |= 8; // poison: do not push a partial/invalid triplet
                return;
            }
            m_gainsAccum[channel] = factor;
            m_gainMask |= bit;
            if ((m_gainMask & 0b111) == 0b111 && !(m_gainMask & 8)) {
                onRGBGainsChanged(m_gainsAccum[0], m_gainsAccum[1], m_gainsAccum[2]);
            }
        } else if (kind == QLatin1String("appRules")) {
            // Per-app profiles: an effect older than this feature answers
            // UnknownMethod — hide the UI instead of pretending the rules apply.
            setProfilesSupported(!isError);
            if (!isError) {
                onAppRulesChanged(sanitizeRuleList(args.value(0).toStringList()));
                // Fine-grained capability: profiles with per-channel gains and
                // display names need the newer setAppRuleFull/setAppRuleName.
                QDBusMessage introspect = QDBusMessage::createMethodCall(
                    m_serviceOwner,
                    QString::fromLatin1(s_vibrancePath),
                    QStringLiteral("org.freedesktop.DBus.Introspectable"),
                    QStringLiteral("Introspect"));
                const QDBusPendingCall call =
                    QDBusConnection::sessionBus().asyncCall(introspect, kRefreshTimeoutMs);
                auto *intro = new QDBusPendingCallWatcher(call, this);
                intro->setProperty("kind", QStringLiteral("__introspect"));
                intro->setProperty("seq", m_refreshSeq);
                connect(intro, &QDBusPendingCallWatcher::finished,
                        this, &VibranceController::onRefreshReply);
            } else {
                m_profilesFull = false;
            }
        } else if (kind == QLatin1String("__introspect")) {
            m_profilesFull = !isError && reply.arguments().value(0).toString()
                                          .contains(QLatin1String("setAppRuleFull"));
        }
    }

    /** What the global hotkey runs: an atomic, server-side flip. */
    void toggleVibrance()
    {
        if (!m_attached) {
            // Not connected yet: bring the effect up; the next press toggles.
            ensureEffectLoaded();
            return;
        }
        // The effect flips its own state — no cached copy to go stale.
        send(QStringLiteral("toggleEnabled"));
        // Belt and braces: re-pull state shortly after so every checkbox and
        // the tooltip reflect reality even if the signal path misbehaves.
        QTimer::singleShot(160, this, &VibranceController::refreshState);
    }

private:
    /** Fire-and-forget call to the verified effect connection. Addressing
     *  the unique owner (not the well-known name) means a later owner swap
     *  can never receive our requests, and no synchronous introspection is
     *  involved. */
    void send(const QString &method, const QVariantList &args = {})
    {
        if (!m_attached) {
            return;
        }
        QDBusMessage msg = QDBusMessage::createMethodCall(
            m_serviceOwner,
            QString::fromLatin1(s_vibrancePath),
            QString::fromLatin1(s_vibranceInterface),
            method);
        msg.setArguments(args);
        QDBusConnection::sessionBus().asyncCall(msg, kRefreshTimeoutMs);
    }

    void setProfilesSupported(bool supported)
    {
        if (supported == m_profilesSupported) {
            return;
        }
        m_profilesSupported = supported;
        Q_EMIT profilesSupportChanged(supported);
    }

    void flushRGB()
    {
        if (!m_attached || !m_rgbPending) {
            return;
        }
        m_rgbPending = false;
        send(QStringLiteral("setRGBGains"), {m_pendingRed, m_pendingGreen, m_pendingBlue});
    }
    void flush()
    {
        if (!m_attached || m_pendingPercent < 0) {
            return;
        }
        const double value = m_pendingPercent / 100.0;
        m_pendingPercent = -1;
        if (qFuzzyCompare(value, m_lastSent)) {
            return;
        }
        m_lastSent = value;
        send(QStringLiteral("setVibrance"), {value});
    }

    void attachInterface()
    {
        if (m_attached) {
            return;
        }
        const QString owner = serviceOwnerOf(QString::fromLatin1(s_vibranceService));
        if (owner.isEmpty()) {
            return; // not running (yet); serviceRegistered will bring us back
        }
        m_serviceOwner = owner;
        // Trust is anchored to the process, not the name: whoever grabbed
        // org.wayvibrance must BE the compositor running our effect.
        if (!ownerIsCompositor(m_serviceOwner)) {
            qWarning().noquote() << "Way Vibrance: org.wayvibrance is owned by a"
                                 << "non-compositor process; ignoring it";
            // Do not keep a rejected identity: if it lingers, a later swap
            // back to the same non-compositor value would compare equal and
            // skip the retry forever.
            m_serviceOwner.clear();
            return;
        }
        // No QDBusInterface here on purpose: its constructor introspects the
        // service synchronously (Qt's 25 s default timeout), which would undo
        // the bounded-probe design. Every call goes to this unique owner via
        // send()/asyncCall instead.
        m_attached = true;

        auto bus = QDBusConnection::sessionBus();
        // One generic subscription instead of five typed ones: with this
        // Qt/dbus-broker pair the typed string-based connects never deliver
        // (they register fine, then silently drop), while the empty-member
        // QDBusMessage form works and stays version-proof for new signals.
        // The empty service means "any sender" — path+interface already pin
        // it to our effect.
        bus.connect(QString(),
                    QString::fromLatin1(s_vibrancePath),
                    QString::fromLatin1(s_vibranceInterface),
                    QString(),
                    this,
                    SLOT(onSignal(QDBusMessage)));

        // Apply anything the user changed while the effect was away, then
        // refresh so the UI reconciles with whatever the compositor reports.
        flush();
        flushRGB();
        refreshState();
        Q_EMIT availableChanged(true);
    }

    void detachInterface()
    {
        if (!m_attached) {
            return;
        }
        auto bus = QDBusConnection::sessionBus();
        bus.disconnect(QString(),
                       QString::fromLatin1(s_vibrancePath),
                       QString::fromLatin1(s_vibranceInterface),
                       QString(),
                       this,
                       SLOT(onSignal(QDBusMessage)));
        ++m_refreshSeq; // drop any in-flight refresh replies with the old owner
        ++m_lastFocusedSeq; // and any last-focused reply from the old owner
        m_attached = false;
        m_lastSent = -1.0;
        m_lastFocusedPending = false;
        m_lastFocusedCb = nullptr;
        setProfilesSupported(false);
        // Capability is per effect build, not per tray: a reattach may land
        // on an older effect, and a stale "full" flag would make us send
        // setAppRuleFull to an interface that only knows setAppRule.
        m_profilesFull = false;
        Q_EMIT availableChanged(false);
    }

    void onServiceRegistered(const QString &name)
    {
        if (name == QLatin1String(s_vibranceService)) {
            attachInterface();
        }
    }

    void onServiceUnregistered(const QString &name)
    {
        if (name == QLatin1String(s_vibranceService)) {
            detachInterface();
        }
    }

    bool m_attached = false; // talking to a verified effect connection
    QString m_serviceOwner; // unique bus name of the org.wayvibrance owner
    quint64 m_refreshSeq = 0;
    quint64 m_lastFocusedSeq = 0;
    bool m_lastFocusedPending = false;
    std::function<void(const QString &)> m_lastFocusedCb; // invalidates in-flight refreshes on detach/new round
    double m_gainsAccum[3] = {1.0, 1.0, 1.0};
    int m_gainMask = 0; // bits 0-2 arrived (r,g,b); bit 3 = poisoned
    QTimer m_timer;
    int m_pendingPercent = -1;
    double m_lastSent = -1.0;
    QTimer m_rgbTimer;
    double m_pendingRed = 1.0;
    double m_pendingGreen = 1.0;
    double m_pendingBlue = 1.0;
    bool m_rgbPending = false;
    QAction *m_toggleAction = nullptr;
    QKeySequence m_toggleShortcut;
    bool m_shortcutsReady = false;
    bool m_externalsTrusted = false;
    bool m_profilesSupported = false;
    bool m_profilesFull = false;
    bool m_primaryOutputSupported = false;
    QDBusServiceWatcher m_watcher;
};

// Behaviour/settings helpers — implemented further below, used by the window.
static bool autostartEnabled();
static void setAutostartEnabled(bool enabled);
static bool storedStartHidden();
static void saveStartHidden(bool hidden);

/**
 * Splits a serialized profile ("app:firefox|1.25[|r,g,b]") into the
 * prefixed pattern (as setAppRule expects), the vibrance percent and the
 * per-channel percents (default 100/100/100 when the profile has none).
 */
struct ProfileValues
{
    QString prefixedPattern;
    QString displayName;
    int vibrancePercent = 100;
    int redPercent = 100;
    int greenPercent = 100;
    int bluePercent = 100;
    bool hasGains = false;
};

static bool parseProfileEntry(const QString &entry, ProfileValues &out)
{
    // kind:pattern|vib[|r,g,b[|name]] — fields are positional; patterns and
    // names are guaranteed never to contain '|'.
    const QStringList fields = entry.split(QLatin1Char('|'));
    if (fields.size() < 2 || fields.size() > 4) {
        return false;
    }
    bool ok = false;
    const double vib = fields.at(1).toDouble(&ok);
    if (!ok || !std::isfinite(vib)) {
        return false;
    }
    if (fields.at(0).size() > kMaxPatternLength + 6) { // +6 covers the app:/title: prefix
        return false;
    }
    out.prefixedPattern = fields.at(0);
    out.vibrancePercent = qBound(0, static_cast<int>(qRound(qBound(0.0, vib, 2.0) * 100.0)), 200);
    if (fields.size() >= 3) {
        const QStringList gains = fields.at(2).split(QLatin1Char(','));
        if (gains.size() != 3) {
            return false;
        }
        const double r = gains.at(0).toDouble(&ok);
        if (!ok) {
            return false;
        }
        const double g = gains.at(1).toDouble(&ok);
        if (!ok) {
            return false;
        }
        const double b = gains.at(2).toDouble(&ok);
        if (!ok) {
            return false;
        }
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
            return false;
        }
        out.redPercent = qBound(0, static_cast<int>(qRound(qBound(0.0, r, 2.0) * 100.0)), 200);
        out.greenPercent = qBound(0, static_cast<int>(qRound(qBound(0.0, g, 2.0) * 100.0)), 200);
        out.bluePercent = qBound(0, static_cast<int>(qRound(qBound(0.0, b, 2.0) * 100.0)), 200);
        out.hasGains = out.redPercent != 100 || out.greenPercent != 100 || out.bluePercent != 100;
    }
    if (fields.size() == 4) {
        out.displayName = fields.at(3).left(kMaxNameLength);
    }
    return true;
}

/**
 * "org.kde.dolphin" -> "Dolphin", "steam_app_730" -> "Steam app 730".
 * Globs stay verbatim (they are the meaningful part), title patterns keep
 * their prefix so the user sees what kind of rule it is.
 */
static QString friendlyAppName(const QString &prefixedPattern)
{
    const bool titleRule = prefixedPattern.startsWith(QLatin1String("title:"));
    const QString body = prefixedPattern.mid(titleRule ? 6 : 4);
    if (titleRule || body.contains(QLatin1Char('*')) || body.contains(QLatin1Char('?'))) {
        return prefixedPattern;
    }
    QString name = body.section(QLatin1Char('.'), -1);
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!name.isEmpty()) {
        name = name.at(0).toUpper() + name.mid(1);
    }
    return name;
}

/**
 * Add/edit dialog for a per-app profile — its own window, mirroring the
 * global controls. Apply pushes the values to the effect immediately and
 * keeps the dialog open; OK closes it, asking to apply first when there
 * are unapplied edits.
 *
 * When the loaded effect predates the per-profile gains/display-name API
 * (fullApi == false), those sections are hidden instead of silently doing
 * nothing: pushes then fall back to plain setAppRule(vibrance).
 */
class ProfileDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProfileDialog(bool fullApi, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(tr("Per-app profile"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);

        auto *matchRow = new QHBoxLayout();
        m_kind = new QComboBox(this);
        m_kind->addItem(tr("App ID / class"));
        m_kind->addItem(tr("Window title"));
        m_pattern = new QLineEdit(this);
        m_pattern->setMaxLength(kMaxPatternLength);
        m_pattern->setPlaceholderText(QStringLiteral("firefox, *steam*, org.kde.dolphin…"));
        matchRow->addWidget(m_kind);
        matchRow->addWidget(m_pattern, 1);
        layout->addLayout(matchRow);
        connect(m_kind, &QComboBox::currentIndexChanged, this, [this] { markDirty(); });

        if (fullApi) {
            auto *nameRow = new QHBoxLayout();
            nameRow->addWidget(new QLabel(tr("Display name"), this));
            m_name = new QLineEdit(this);
            m_name->setMaxLength(kMaxNameLength);
            m_name->setPlaceholderText(tr("cosmetic — matching still uses the pattern"));
            nameRow->addWidget(m_name, 1);
            layout->addLayout(nameRow);
            connect(m_name, &QLineEdit::textChanged, this, [this] { markDirty(); });
        }

        // Same card treatment as the RGB gains below, so the slider,
        // percentage column and reset buttons line up across both sections.
        auto *vibranceBox = new QGroupBox(tr("Vibrance of matching windows"), this);
        auto *vibranceLayout = new QVBoxLayout(vibranceBox);
        vibranceLayout->setSpacing(6);
        makeSliderRow(vibranceLayout, QString(), kMinPercent, m_vibSlider, m_vibValue);

        // Quick values, same set as the global presets row: one exclusive
        // segmented control. A click still goes through QSlider so the
        // existing dirty/label/apply path is untouched.
        m_presetGroup = new QButtonGroup(this);
        m_presetGroup->setExclusive(true);
        auto *presetRow = new QHBoxLayout();
        presetRow->setSpacing(0);
        // Two passes so every segment ends up the exact same width: the
        // widest label sets the shared minimum, then stretch 1 distributes
        // the rest equally (no text-length jitter between 0% and 200%).
        QList<QPushButton *> dialogPresets;
        dialogPresets.reserve(wvseg::kPresetCount);
        for (int i = 0; i < wvseg::kPresetCount; ++i) {
            const int percent = wvseg::kPresetValues[i];
            auto *presetButton = wvseg::createPresetButton(
                vibranceBox, QStringLiteral("%1%").arg(percent), i == 0,
                i == wvseg::kPresetCount - 1);
            presetButton->setProperty("wvPresetValue", percent);
            m_presetGroup->addButton(presetButton);
            connect(presetButton, &QPushButton::clicked, this, [this, percent] {
                m_vibSlider->setValue(percent); // the valueChanged path marks dirty and refreshes the label
                // An unchanged value emits no valueChanged, so a click on the
                // already-active preset must still re-assert the group state.
                wvseg::syncPresetGroup(m_presetGroup, m_vibSlider->value());
            });
            dialogPresets.append(presetButton);
        }
        int maxPresetWidth = 0;
        for (auto *b : dialogPresets) {
            maxPresetWidth = qMax(maxPresetWidth, b->sizeHint().width());
        }
        for (auto *b : dialogPresets) {
            b->setMinimumWidth(maxPresetWidth);
            presetRow->addWidget(b, 1);
        }
        vibranceLayout->addLayout(presetRow);
        connect(m_vibSlider, &QSlider::valueChanged, this, [this](int percent) {
            wvseg::syncPresetGroup(m_presetGroup, percent);
        });
        wvseg::syncPresetGroup(m_presetGroup, m_vibSlider->value());
        layout->addWidget(vibranceBox);

        if (fullApi) {
            m_gainsBox = new QGroupBox(tr("Channel gains (these windows only)"), this);
            auto *gainsLayout = new QVBoxLayout(m_gainsBox);
            gainsLayout->setSpacing(6);
            makeSliderRow(gainsLayout, QStringLiteral("R"), kRgbMinPercent, m_redSlider, m_redValue);
            makeSliderRow(gainsLayout, QStringLiteral("G"), kRgbMinPercent, m_greenSlider, m_greenValue);
            makeSliderRow(gainsLayout, QStringLiteral("B"), kRgbMinPercent, m_blueSlider, m_blueValue);
            auto *resetRgb = new QPushButton(tr("Reset RGB"), m_gainsBox);
            connect(resetRgb, &QPushButton::clicked, this, [this] {
                m_redSlider->setValue(100);
                m_greenSlider->setValue(100);
                m_blueSlider->setValue(100);
            });
            gainsLayout->addWidget(resetRgb);
            layout->addWidget(m_gainsBox);
        }

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->addButton(QDialogButtonBox::Apply);
        m_okButton = buttons->button(QDialogButtonBox::Ok);
        m_applyButton = buttons->button(QDialogButtonBox::Apply);
        m_applyButton->setEnabled(false);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_applyButton, &QAbstractButton::clicked, this, [this] {
            if (m_dirty) {
                emit applyRequested();
                m_dirty = false;
                updateButtons();
            }
        });
        connect(m_pattern, &QLineEdit::textChanged, this, [this] { validate(); markDirty(); });

        resize(460, fullApi ? 380 : 260);
        updateButtons();
    }

    void setFields(bool titleKind, const QString &pattern, int vibrancePercent,
                   int redPercent = 100, int greenPercent = 100, int bluePercent = 100,
                   const QString &displayName = QString())
    {
        m_suppressDirty = true;
        m_kind->setCurrentIndex(titleKind ? 1 : 0);
        m_pattern->setText(pattern);
        if (m_name) {
            m_name->setText(displayName);
        }
        m_vibSlider->setValue(qBound(kMinPercent, vibrancePercent, kMaxPercent));
        if (m_redSlider) {
            m_redSlider->setValue(qBound(kRgbMinPercent, redPercent, kRgbMaxPercent));
            m_greenSlider->setValue(qBound(kRgbMinPercent, greenPercent, kRgbMaxPercent));
            m_blueSlider->setValue(qBound(kRgbMinPercent, bluePercent, kRgbMaxPercent));
        }
        m_suppressDirty = false;
        // setValue() only emits valueChanged when the value actually
        // changes, so the group needs an explicit sync for a no-op edit.
        wvseg::syncPresetGroup(m_presetGroup, m_vibSlider->value());
        validate();
        updateButtons();
    }

    /**
     * Whether the rule this dialog represents already exists on the effect
     * (edit of an existing profile) or was created by a previous Apply here.
     * A never-applied dialog saves silently on OK instead of asking about
     * "changes": pressing OK on a brand-new profile *is* the intent to save.
     */
    void setAppliedOnce(bool applied) { m_appliedOnce = applied; }

    /** The pattern with its kind prefix, as the effect's setAppRule takes it. */
    QString prefixedPattern() const
    {
        const QString body = m_pattern->text().trimmed();
        return (m_kind->currentIndex() == 1 ? QStringLiteral("title:") : QStringLiteral("app:")) + body;
    }

    double ruleVibrance() const { return m_vibSlider->value() / 100.0; }
    double ruleRed() const { return m_redSlider ? m_redSlider->value() / 100.0 : 1.0; }
    double ruleGreen() const { return m_greenSlider ? m_greenSlider->value() / 100.0 : 1.0; }
    double ruleBlue() const { return m_blueSlider ? m_blueSlider->value() / 100.0 : 1.0; }
    QString ruleName() const { return m_name ? m_name->text().trimmed() : QString(); }
    bool hasGainsUi() const { return m_redSlider != nullptr; }

Q_SIGNALS:
    void applyRequested();

protected:
    void done(int result) override
    {
        if (result == Accepted) {
            if (!m_appliedOnce) {
                // Brand-new profile (nothing on the effect yet): OK means
                // save, quietly — asking "apply changes?" would be noise.
                emit applyRequested();
            } else if (m_dirty) {
                const QMessageBox::StandardButton answer = QMessageBox::question(
                    this, tr("Apply changes?"),
                    tr("There are unapplied changes. Apply them before closing?"),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
                if (answer == QMessageBox::Cancel) {
                    return;
                }
                if (answer == QMessageBox::Yes) {
                    emit applyRequested();
                }
            }
        }
        QDialog::done(result);
    }

private:
    void markDirty()
    {
        if (m_suppressDirty) {
            return;
        }
        m_dirty = true;
        updateButtons();
    }

    void updateButtons()
    {
        m_applyButton->setEnabled(m_dirty);
    }

    void makeSliderRow(QVBoxLayout *layout, const QString &channel,
                       int minimum, QSlider *&slider, QLabel *&value)
    {
        auto *row = new QHBoxLayout();
        if (!channel.isEmpty()) {
            auto *label = new QLabel(channel, this);
            // The application sheet owns the semantic colour; keeping it out
            // of an inline stylesheet is what lets a scheme switch re-polish
            // the label with the rest of the window.
            label->setProperty("wvChannel", channel);
            label->setFixedWidth(12);
            row->addWidget(label);
        }
        slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(minimum, kMaxPercent);
        slider->setValue(100);
        if (!channel.isEmpty()) {
            slider->setProperty("wvChannel", channel);
        }
        value = createSliderValueLabel(this, kMaxPercent);
        value->setText(QStringLiteral("100%"));
        row->addWidget(slider, 1);
        row->addWidget(value, 0, Qt::AlignVCenter);
        row->addWidget(createResetButton(this, slider, 100), 0, Qt::AlignVCenter);
        layout->addLayout(row);
        connect(slider, &QSlider::valueChanged, this, [this, value](int percent) {
            value->setText(QStringLiteral("%1%").arg(percent));
            markDirty();
        });
    }

    void validate()
    {
        const QString body = m_pattern->text().trimmed();
        // '|' is our serialization separator — patterns containing it would
        // round-trip wrongly, so they are not accepted (the effect rejects
        // them too).
        m_okButton->setEnabled(!body.isEmpty() && !body.contains(QLatin1Char('|')));
    }

    QComboBox *m_kind = nullptr;
    QLineEdit *m_pattern = nullptr;
    QGroupBox *m_gainsBox = nullptr;
    QLineEdit *m_name = nullptr;
    QButtonGroup *m_presetGroup = nullptr;
    QSlider *m_vibSlider = nullptr;
    QLabel *m_vibValue = nullptr;
    QSlider *m_redSlider = nullptr;
    QLabel *m_redValue = nullptr;
    QSlider *m_greenSlider = nullptr;
    QLabel *m_greenValue = nullptr;
    QSlider *m_blueSlider = nullptr;
    QLabel *m_blueValue = nullptr;
    QPushButton *m_okButton = nullptr;
    QAbstractButton *m_applyButton = nullptr;
    bool m_dirty = false;
    bool m_suppressDirty = false;
    bool m_appliedOnce = false;
};

// ---------------------------------------------------------------------------
// Update checking helpers
// ---------------------------------------------------------------------------

// Version string of this build, injected by CMake (single source of truth:
// project() VERSION in CMakeLists.txt).
#ifndef WAVERBANCE_VERSION
#define WAVERBANCE_VERSION "0.0.0"
#endif

/** Best-effort description of how this build was installed, used to tell the
 *  user *where* to update (we deliberately never run a package manager on
 *  their behalf: that is the distro's job, not the app's). */
static QString installMethodHint()
{
    if (qEnvironmentVariableIsSet("FLATPAK_ID") || QFile::exists(QStringLiteral("/.flatpak-info"))) {
        return QObject::tr("This copy is a Flatpak. Update it with "
                           "<code>flatpak update</code> or your software center.");
    }
    if (qEnvironmentVariableIsSet("APPIMAGE")) {
        return QObject::tr("This copy is an AppImage. Download the new AppImage "
                           "from the release page below.");
    }
    if (qEnvironmentVariableIsSet("SNAP")) {
        return QObject::tr("This copy is a Snap. Update it with "
                           "<code>sudo snap refresh</code> or your software center.");
    }

    QString id;
    QStringList idLike;
    QFile osRelease(QStringLiteral("/etc/os-release"));
    if (osRelease.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString text = QString::fromUtf8(osRelease.readAll());
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("ID="))) {
                id = line.mid(3).remove(QLatin1Char('"')).trimmed();
            } else if (line.startsWith(QLatin1String("ID_LIKE="))) {
                idLike = line.mid(8)
                             .remove(QLatin1Char('"'))
                             .trimmed()
                             .split(QLatin1Char(' '), Qt::SkipEmptyParts);
            }
        }
    }

    // Match either the exact ID or a parent distro declared via ID_LIKE, so
    // derivatives (Zorin, Kali, Nobara, …) still get the right command.
    const auto familyIs = [&](const QStringList &names) {
        if (names.contains(id)) {
            return true;
        }
        for (const QString &parent : idLike) {
            if (names.contains(parent)) {
                return true;
            }
        }
        return false;
    };

    if (familyIs({QStringLiteral("arch"), QStringLiteral("manjaro"),
                  QStringLiteral("endeavouros")})) {
        return QObject::tr("Installed from a package. Update with your package "
                           "manager, e.g. <code>sudo pacman -Syu</code>.");
    }
    if (QFile::exists(QStringLiteral("/run/ostree-booted"))) {
        return QObject::tr("This is an rpm-ostree (Atomic) system. Update with "
                           "<code>rpm-ostree update</code>, then reboot.");
    }
    if (familyIs({QStringLiteral("fedora"), QStringLiteral("rhel"),
                  QStringLiteral("centos")})) {
        return QObject::tr("Installed from a package. Update with "
                           "<code>sudo dnf upgrade</code>.");
    }
    if (familyIs({QStringLiteral("opensuse"), QStringLiteral("suse"),
                  QStringLiteral("sles")})
        || id.startsWith(QLatin1String("opensuse"))) {
        return QObject::tr("Installed from a package. Update with "
                           "<code>sudo zypper update</code>.");
    }
    if (familyIs({QStringLiteral("debian"), QStringLiteral("ubuntu"),
                  QStringLiteral("linuxmint")})) {
        return QObject::tr("Installed from a package. Update with "
                           "<code>sudo apt update &amp;&amp; sudo apt upgrade</code>.");
    }
    return QObject::tr("If this build came from a package, update it through "
                       "your distribution's package manager.");
}

class TrayWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit TrayWindow(VibranceController *controller, QWidget *parent = nullptr)
        : QMainWindow(parent)
    {
        setWindowTitle(tr("Way Vibrance"));

        // Native menu bar: one "Menu" entry with About and update checking,
        // so both are discoverable from inside the app (the tray context
        // menu exposes the same two actions when the window is hidden).
        auto *appMenu = menuBar()->addMenu(tr("&Menu"));
        appMenu->addAction(QIcon::fromTheme(QStringLiteral("help-about")),
                           tr("About Way Vibrance"), this, &TrayWindow::showAbout);
        appMenu->addAction(QIcon::fromTheme(QStringLiteral("system-software-update")),
                           tr("Check for Updates…"), this, &TrayWindow::checkForUpdates);

        auto *central = new QWidget(this);

        // Two panes: the global controls on the left, the per-app profile
        // editor on the right — a distinct "section" of the app that hides
        // (and lets the window collapse back) when unsupported.
        auto *columns = new QHBoxLayout(central);
        columns->setContentsMargins(16, 16, 16, 16);
        columns->setSpacing(18);
        auto *outerLayout = new QVBoxLayout();
        // Tight enough that the whole window (including the extra "Load
        // effect" row) still fits a 1366x768 work area, which the first
        // card layout overshot by a few pixels.
        outerLayout->setSpacing(8);
        columns->addLayout(outerLayout, 5);
        m_sidePane = new QWidget(central);
        auto *sideLayout = new QVBoxLayout(m_sidePane);
        sideLayout->setContentsMargins(0, 0, 0, 0);
        sideLayout->setSpacing(12);
        columns->addWidget(m_sidePane, 4);

        m_statusLabel = new QLabel(central);
        outerLayout->addWidget(m_statusLabel);

        // The primary slider lives in a card like every other section, so
        // its percentage column and reset button line up with the RGB rows
        // instead of extending to the window edge.
        auto *vibranceGroup = new QGroupBox(tr("Vibrance"), central);
        auto *vibranceLayout = new QVBoxLayout(vibranceGroup);
        vibranceLayout->setSpacing(6);

        auto *row = new QHBoxLayout();
        m_slider = new QSlider(Qt::Horizontal, vibranceGroup);
        m_slider->setRange(kMinPercent, kMaxPercent);
        m_slider->setValue(100);
        m_valueLabel = createSliderValueLabel(vibranceGroup, kMaxPercent);
        // The slider starts at 100 but does not emit valueChanged for its
        // initial value; without this the row shows an empty gap until the
        // first state push (and forever when the effect is unavailable).
        m_valueLabel->setText(QStringLiteral("100%"));
        row->addWidget(m_slider, 1);
        row->addWidget(m_valueLabel, 0, Qt::AlignVCenter);
        row->addWidget(createResetButton(vibranceGroup, m_slider, 100), 0, Qt::AlignVCenter);
        vibranceLayout->addLayout(row);

        m_enabledCheck = new wvswitch::Switch(tr("Enabled"), vibranceGroup);
        m_enabledCheck->setChecked(true);
        vibranceLayout->addWidget(m_enabledCheck);

        m_presetGroup = new QButtonGroup(this);
        m_presetGroup->setExclusive(true);
        auto *presets = new QHBoxLayout();
        presets->setSpacing(0);
        // Two passes so every segment ends up the exact same width (no
        // text-length jitter between 0% and 200%).
        QList<QPushButton *> trayPresets;
        trayPresets.reserve(wvseg::kPresetCount);
        for (int i = 0; i < wvseg::kPresetCount; ++i) {
            const int percent = wvseg::kPresetValues[i];
            auto *button = wvseg::createPresetButton(
                vibranceGroup, QStringLiteral("%1%").arg(percent), i == 0,
                i == wvseg::kPresetCount - 1);
            button->setProperty("wvPresetValue", percent);
            m_presetGroup->addButton(button);
            connect(button, &QPushButton::clicked, this, [this, percent] {
                const int before = m_slider->value();
                setPercent(percent); // also re-syncs the segmented state
                // setValue() already requests the percent through the
                // valueChanged path when the value actually changes; asking
                // again would double the D-Bus traffic. Only the
                // already-active preset (no valueChanged) needs this
                // explicit re-assert against the effect.
                if (percent == before && m_controller) {
                    m_controller->requestVibrancePercent(percent);
                }
            });
            trayPresets.append(button);
        }
        int maxTrayPresetWidth = 0;
        for (auto *b : trayPresets) {
            maxTrayPresetWidth = qMax(maxTrayPresetWidth, b->sizeHint().width());
        }
        for (auto *b : trayPresets) {
            b->setMinimumWidth(maxTrayPresetWidth);
            presets->addWidget(b, 1);
        }
        vibranceLayout->addLayout(presets);
        wvseg::syncPresetGroup(m_presetGroup, m_slider->value());
        outerLayout->addWidget(vibranceGroup);

        // Modern UI refinements: consistent margins and spacing
        outerLayout->setContentsMargins(12, 12, 12, 12);
        outerLayout->setSpacing(16);

        // Per-channel RGB gains (like the driver's color page). Percent
        // values map to gain multipliers (100% = neutral 1.0).
        auto *rgbGroup = new QGroupBox(tr("Channel gains (RGB)"), central);
        // Flat & Airy: removing the group box border, adding consistent radius for modern UI
        rgbGroup->setStyleSheet(QStringLiteral("QGroupBox { border: none; font-weight: bold; } QPushButton { border-radius: 4px; }"));
        auto *rgbLayout = new QVBoxLayout(rgbGroup);
        rgbLayout->setContentsMargins(0, 8, 0, 0);
        rgbLayout->setSpacing(6);
        struct ChannelSpec
        {
            const char *name;
            QSlider *TrayWindow::*member;
            QLabel *TrayWindow::*label;
        };
        const ChannelSpec channels[] = {
            {"R", &TrayWindow::m_redSlider, &TrayWindow::m_redLabel},
            {"G", &TrayWindow::m_greenSlider, &TrayWindow::m_greenLabel},
            {"B", &TrayWindow::m_blueSlider, &TrayWindow::m_blueLabel},
        };
        for (const ChannelSpec &ch : channels) {
            auto *row = new QHBoxLayout();
            auto *name = new QLabel(QString::fromLatin1(ch.name), rgbGroup);
            // Semantic colour comes from the application sheet, adapted to
            // the current palette so it stays legible on light cards.
            name->setProperty("wvChannel", QString::fromLatin1(ch.name));
            auto *slider = new QSlider(Qt::Horizontal, rgbGroup);
            slider->setRange(kRgbMinPercent, kMaxPercent);
            slider->setValue(100);
            slider->setProperty("wvChannel", QString::fromLatin1(ch.name));
            auto *value = createSliderValueLabel(rgbGroup, kMaxPercent);
            value->setText(QStringLiteral("100%"));
            row->addWidget(name);
            row->addWidget(slider, 1);
            row->addWidget(value, 0, Qt::AlignVCenter);
            row->addWidget(createResetButton(rgbGroup, slider, 100), 0, Qt::AlignVCenter);
            rgbLayout->addLayout(row);
            this->*(ch.member) = slider;
            this->*(ch.label) = value;
            connect(slider, &QSlider::valueChanged, this, [this, value](int percent) {
                value->setText(QStringLiteral("%1%").arg(percent));
                pushRGBGains();
            });
        }
        auto *rgbReset = new QPushButton(tr("Reset RGB"), rgbGroup);
        connect(rgbReset, &QPushButton::clicked, this, [this] {
            const bool alreadyNeutral = m_redSlider->value() == 100
                && m_greenSlider->value() == 100 && m_blueSlider->value() == 100;
            setGains(100, 100, 100);
            // As with the presets above: each changed slider already pushes
            // its gain through valueChanged, so the explicit request is
            // only needed when everything was already neutral.
            if (alreadyNeutral && m_controller) {
                m_controller->requestRGBGains(1.0, 1.0, 1.0);
            }
        });
        rgbLayout->addWidget(rgbReset);
        outerLayout->addWidget(rgbGroup);

        // Per-app profiles (the counterpart of vibrance-gui's per-game
        // settings): matching windows override the global vibrance — and can
        // carry their own per-channel RGB gains. Add/edit opens its own
        // window, mirroring the global controls.
        m_profileGroup = new QGroupBox(tr("Per-app profiles"), m_sidePane);
        // Flat & Airy: removing the group box border, adding consistent radius for modern UI
        m_profileGroup->setStyleSheet(QStringLiteral("QGroupBox { border: none; font-weight: bold; }"));
        // Applying border radius to buttons and inputs within to match Plasma 6 style
        m_profileGroup->setStyleSheet(QStringLiteral("QGroupBox { border: none; font-weight: bold; } QPushButton { border-radius: 4px; } QLineEdit { border-radius: 4px; } QComboBox { border-radius: 4px; }"));
        auto *profileLayout = new QVBoxLayout(m_profileGroup);
        profileLayout->setContentsMargins(0, 8, 0, 0);
        profileLayout->setSpacing(8);

        // Empty state: without a rule the pane used to be a large blank box.
        // A stacked page explains what the pane is for and gives way to the
        // list as soon as the first profile arrives.
        m_profileStack = new QStackedWidget(m_profileGroup);
        auto *emptyPage = new QWidget(m_profileStack);
        auto *emptyLayout = new QVBoxLayout(emptyPage);
        emptyLayout->setContentsMargins(16, 16, 16, 16);
        emptyLayout->addStretch(1);
        auto *emptyIcon = new QLabel(emptyPage);
        QIcon folderIcon = QIcon::fromTheme(QStringLiteral("folder-open"));
        if (folderIcon.isNull()) {
            folderIcon = style()->standardIcon(QStyle::SP_DirOpenIcon);
        }
        // 48 logical pixels, rendered at the widget's device pixel ratio:
        // a 1x pixmap would be upscaled and blurry on a 2x display.
        const qreal emptyIconDpr = emptyIcon->devicePixelRatioF();
        QPixmap folderPixmap = folderIcon.pixmap(qRound(48 * emptyIconDpr),
                                                  qRound(48 * emptyIconDpr));
        folderPixmap.setDevicePixelRatio(emptyIconDpr);
        emptyIcon->setPixmap(folderPixmap);
        emptyIcon->setAlignment(Qt::AlignCenter);
        emptyLayout->addWidget(emptyIcon, 0, Qt::AlignHCenter);
        auto *emptyTitle = new QLabel(tr("No profiles yet"), emptyPage);
        QFont emptyTitleFont = emptyTitle->font();
        emptyTitleFont.setBold(true);
        emptyTitle->setFont(emptyTitleFont);
        emptyTitle->setAlignment(Qt::AlignCenter);
        emptyLayout->addWidget(emptyTitle, 0, Qt::AlignHCenter);
        auto *emptyHint = new QLabel(
            tr("Add an application below to give it its own vibrance and RGB gains."), emptyPage);
        emptyHint->setProperty("wvHint", true);
        emptyHint->setAlignment(Qt::AlignCenter);
        emptyHint->setWordWrap(true);
        emptyHint->setMinimumWidth(200); // Garante espaço, mas permite que o layout controle
        emptyHint->setMaximumWidth(300); // Restringe a largura para não ficar muito estreito
        emptyHint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        emptyLayout->addWidget(emptyHint, 0, Qt::AlignHCenter);
        emptyLayout->addStretch(1);

        m_profileList = new QListWidget(m_profileStack);
        m_profileList->setMinimumWidth(280);
        m_profileList->setToolTip(tr("Double-click a profile to edit it.\n"
                                     "'app:' patterns match the Wayland app_id / X11 WM_CLASS,\n"
                                     "'title:' patterns match the window caption; '*' and '?' are wildcards."));
        m_profileStack->addWidget(emptyPage);
        m_profileStack->addWidget(m_profileList);
        profileLayout->addWidget(m_profileStack, 1);
        updateProfileEmptyState();

        auto *profileButtons = new QHBoxLayout();
        profileButtons->setContentsMargins(0, 4, 0, 0); // Add top margin to separate from list
        profileButtons->setSpacing(10); // Aumenta o respiro entre botões

        // Botões com ícones e estilo definido para garantir que pareçam botões
        auto *addFocusedButton = new QPushButton(QIcon::fromTheme(QStringLiteral("search")), tr("From last focused…"), m_profileGroup);
        auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add…"), m_profileGroup);
        auto *removeButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), m_profileGroup);

        // Estilo moderno: background definido pela paleta, bordas suaves e padding
        QString btnStyle = QStringLiteral("QPushButton { background: palette(button); border: 1px solid palette(mid); border-radius: 4px; padding: 5px 10px; } "
                                          "QPushButton:hover { background: palette(midlight); }");
        addFocusedButton->setStyleSheet(btnStyle);
        addButton->setStyleSheet(btnStyle);
        removeButton->setStyleSheet(btnStyle);

        addFocusedButton->setToolTip(tr("Pre-fill the app ID of the window you focused before this one."));
        profileButtons->addWidget(addFocusedButton);
        profileButtons->addWidget(addButton);
        profileButtons->addWidget(removeButton);
        profileButtons->addStretch();
        profileLayout->addLayout(profileButtons);
        sideLayout->addWidget(m_profileGroup, 1);

        // A dialog pushes its values through applyRequested(): the Apply
        // button emits it directly, and OK emits it after asking, when
        // something was edited without applying. Renaming a pattern means
        // remove-old + set-new (the pattern is the rule's identity).
        auto pushDialog = [controller](const ProfileDialog &dialog, const QString &originalPattern) {
            const QString pattern = dialog.prefixedPattern();
            if (!originalPattern.isEmpty() && pattern.compare(originalPattern, Qt::CaseInsensitive) != 0) {
                controller->requestRemoveAppRule(originalPattern);
            }
            controller->requestSetAppRuleFull(pattern, dialog.ruleVibrance(),
                                              dialog.ruleRed(), dialog.ruleGreen(), dialog.ruleBlue());
            if (dialog.hasGainsUi()) {
                controller->requestSetAppRuleName(pattern, dialog.ruleName());
            }
        };

        connect(addButton, &QPushButton::clicked, this, [controller, this, pushDialog] {
            ProfileDialog dialog(controller->profilesFullApi(), this);
            // Remember what Apply last committed so a rename-then-Apply
            // replaces its own previous rule instead of orphaning it.
            QString lastApplied;
            QObject::connect(&dialog, &ProfileDialog::applyRequested, &dialog,
                             [&dialog, pushDialog, &lastApplied] {
                pushDialog(dialog, lastApplied);
                lastApplied = dialog.prefixedPattern();
                dialog.setAppliedOnce(true);
            });
            dialog.exec();
        });
        connect(addFocusedButton, &QPushButton::clicked, this, [controller, this, pushDialog] {
            // The controller may deliver the reply after this window is gone;
            // QPointer makes the late callback a no-op instead of a UAF.
            QPointer<TrayWindow> self(this);
            controller->requestLastFocusedAppId([controller, self, pushDialog](const QString &id) {
            if (!self) {
                return;
            }
            ProfileDialog dialog(controller->profilesFullApi(), self.data());
            if (!id.isEmpty()) {
                dialog.setFields(false, id, 125);
            }
            QString lastApplied;
            QObject::connect(&dialog, &ProfileDialog::applyRequested, &dialog,
                             [&dialog, pushDialog, &lastApplied] {
                pushDialog(dialog, lastApplied);
                lastApplied = dialog.prefixedPattern();
                dialog.setAppliedOnce(true);
            });
            dialog.exec();
            });
        });
        connect(removeButton, &QPushButton::clicked, this, [controller, this] {
            const QString pattern = currentProfilePattern();
            if (!pattern.isEmpty()) {
                controller->requestRemoveAppRule(pattern);
            }
        });
        connect(m_profileList, &QListWidget::itemDoubleClicked, this, [controller, this, pushDialog](QListWidgetItem *item) {
            if (!item) {
                return;
            }
            ProfileValues values;
            if (!parseProfileEntry(item->data(Qt::UserRole).toString(), values)) {
                return;
            }
            const QString oldPattern = values.prefixedPattern;
            const bool titleKind = oldPattern.startsWith(QLatin1String("title:"));
            ProfileDialog dialog(controller->profilesFullApi(), this);
            dialog.setFields(titleKind, oldPattern.mid(titleKind ? 6 : 4), values.vibrancePercent,
                             values.redPercent, values.greenPercent, values.bluePercent, values.displayName);
            // The rule already exists: OK with untouched fields must not
            // re-save (there is nothing new) — only dirty edits matter here.
            dialog.setAppliedOnce(true);
            QString lastApplied = oldPattern;
            QObject::connect(&dialog, &ProfileDialog::applyRequested, &dialog,
                             [&dialog, pushDialog, &lastApplied] {
                pushDialog(dialog, lastApplied);
                lastApplied = dialog.prefixedPattern();
                dialog.setAppliedOnce(true);
            });
            dialog.exec();
        });
        connect(controller, &VibranceController::appRulesFromEffect, this, [this](const QStringList &entries) {
            rebuildProfileList(entries);
        });
        // Profiles only exist on the newer effect build; an older one answers
        // UnknownMethod and we hide the group instead of faking success.
        connect(controller, &VibranceController::profilesSupportChanged, this, [this](bool supported) {
            m_sidePane->setVisible(supported);
        });
        m_sidePane->setVisible(controller->profilesSupported());

        // Configurable global hotkey (KGlobalAccel: persisted by the KDE
        // daemon, editable here and in System Settings → Shortcuts).
        auto *hotkeyGroup = new QGroupBox(tr("Global hotkey"), central);
        // Flat & Airy: removing the group box border, adding consistent radius for modern UI
        hotkeyGroup->setStyleSheet(QStringLiteral("QGroupBox { border: none; font-weight: bold; } QPushButton { border-radius: 4px; } QKeySequenceEdit { border-radius: 4px; }"));
        auto *hotkeyLayout = new QVBoxLayout(hotkeyGroup);
        hotkeyLayout->setSpacing(8);
        auto *hotkeyRow = new QHBoxLayout();
        m_shortcutEdit = new QKeySequenceEdit(hotkeyGroup);
        m_shortcutEdit->setKeySequence(controller->toggleShortcut());
        // Compact icon action: the text button was wider than the shortcut
        // field needs. Falls back to a glyph when the icon theme has no
        // "edit-clear", so this never depends on an external asset.
        auto *clearButton = new QToolButton(hotkeyGroup);
        clearButton->setProperty("wvClear", true);
        clearButton->setCursor(Qt::PointingHandCursor);
        clearButton->setToolTip(tr("Clear the global hotkey"));
        clearButton->setAccessibleName(tr("Clear global hotkey"));
        const QIcon clearIcon = QIcon::fromTheme(QStringLiteral("edit-clear"));
        if (clearIcon.isNull()) {
            clearButton->setText(QStringLiteral("✕"));
        } else {
            clearButton->setIcon(clearIcon);
        }
        hotkeyRow->addWidget(m_shortcutEdit, 1);
        hotkeyRow->addWidget(clearButton, 0, Qt::AlignVCenter);
        hotkeyLayout->addLayout(hotkeyRow);
        m_shortcutInfo = new QLabel(hotkeyGroup);
        hotkeyLayout->addWidget(m_shortcutInfo);
        outerLayout->addWidget(hotkeyGroup);

        auto updateConflictLabel = [this](const QKeySequence &seq) {
            if (seq.isEmpty()) {
                m_shortcutInfo->setText(tr("No hotkey set — click the field and press a key combination."));
                return;
            }
            const QString own = KAboutData::applicationData().componentName();
            for (const KGlobalShortcutInfo &other : KGlobalAccel::globalShortcutsByKey(seq)) {
                if (other.componentUniqueName() == own
                    && other.uniqueName() == QLatin1String(s_toggleActionId)) {
                    continue; // our own binding
                }
                m_shortcutInfo->setText(tr("Warning: already used by \"%1\".")
                                            .arg(other.friendlyName().isEmpty()
                                                     ? other.componentFriendlyName()
                                                     : other.friendlyName()));
                return;
            }
            m_shortcutInfo->setText(tr("Shortcut available."));
        };
        updateConflictLabel(m_shortcutEdit->keySequence());

        connect(m_shortcutEdit, &QKeySequenceEdit::keySequenceChanged, this,
                [this, controller, updateConflictLabel](const QKeySequence &seq) {
                    updateConflictLabel(seq);
                    if (m_updatingShortcut) {
                        return;
                    }
                    // Apply and persist right after the capture — no need
                    // to press Enter or leave the field.
                    controller->registerToggleShortcut(seq);
                });
        connect(clearButton, &QToolButton::clicked, this, [controller, this, updateConflictLabel] {
            m_shortcutEdit->clear();
            controller->registerToggleShortcut(QKeySequence());
            updateConflictLabel(QKeySequence());
        });
        // Mirror genuine external edits (e.g. System Settings) — but ignore
        // anything until the startup wipe/apply ran, and the stale push
        // burst the daemon sends on registration (old builds saved Meta+V
        // there). Otherwise that burst would poison our own config.
        connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this,
                [this, controller](QAction *action, const QKeySequence &seq) {
                    if (action != controller->toggleAction() || !controller->shortcutsReady()) {
                        return;
                    }
                    if (seq != controller->toggleShortcut() && !controller->externalChangesAccepted()) {
                        return; // stale daemon state racing our startup apply
                    }
                    m_updatingShortcut = true;
                    m_shortcutEdit->setKeySequence(seq);
                    m_updatingShortcut = false;
                    VibranceController::saveToggleShortcut(seq);
                });

        m_loadButton = new QPushButton(tr("Load effect into KWin"), central);
        connect(m_loadButton, &QPushButton::clicked, this, [controller] { controller->ensureEffectLoaded(); });
        outerLayout->addWidget(m_loadButton);

        // Behaviour settings — one switch per option, like Enabled. The
        // switch subclass keeps the QCheckBox semantics (role, Space), so
        // the existing toggled() wiring below is unchanged.
        auto *behaviourGroup = new QGroupBox(tr("Behaviour"), central);
        // Flat & Airy: removing the group box border, adding consistent radius for modern UI
        behaviourGroup->setStyleSheet(QStringLiteral("QGroupBox { border: none; font-weight: bold; } QComboBox { border-radius: 4px; }"));
        auto *behaviourLayout = new QVBoxLayout(behaviourGroup);
        // Roomier than the other cards' 6 px: switch rows are a single
        // text line tall, so 4 px read as crowded. 8 px keeps the window
        // inside a 1366x768 work area with every row visible.
        behaviourLayout->setSpacing(8);
        m_startHiddenCheck = new wvswitch::Switch(tr("Open directly to the tray on startup"), behaviourGroup);
        m_startHiddenCheck->setChecked(storedStartHidden());
        m_autostartCheck = new wvswitch::Switch(tr("Start with Plasma (autostart)"), behaviourGroup);
        m_autostartCheck->setChecked(autostartEnabled());
        m_primaryOnlyCheck = new wvswitch::Switch(tr("Only affect the primary monitor"), behaviourGroup);
        // Monitor picker: always visible (when the loaded effect supports
        // choosing the output) but disabled while "primary monitor" mode
        // is off, so toggling the switch never makes the card jump.
        m_primaryOutputRow = new QWidget(behaviourGroup);
        auto *outputRowLayout = new QHBoxLayout(m_primaryOutputRow);
        // Align with the switch rows above: the label starts where the
        // switch text starts (its 4 px focus-ring inset) and the combo
        // ends where the switch track ends (its 2 px edge inset).
        outputRowLayout->setContentsMargins(4, 0, 2, 0);
        outputRowLayout->addWidget(new QLabel(tr("Primary monitor:"), m_primaryOutputRow));
        m_primaryOutputCombo = new QComboBox(m_primaryOutputRow);
        m_primaryOutputCombo->addItem(tr("Automatic (first output)"), QString());
        outputRowLayout->addWidget(m_primaryOutputCombo, 1);
        // Initial state; updateOutputRowState() below keeps it in sync
        // with the switch and with the effect's capability.
        m_primaryOutputRow->setVisible(false);
        m_primaryOutputRow->setEnabled(false);
        behaviourLayout->addWidget(m_startHiddenCheck);
        behaviourLayout->addWidget(m_autostartCheck);
        behaviourLayout->addWidget(m_primaryOnlyCheck);
        behaviourLayout->addWidget(m_primaryOutputRow);
        outerLayout->addWidget(behaviourGroup);

        connect(m_startHiddenCheck, &QCheckBox::toggled, this, [](bool hidden) {
            saveStartHidden(hidden);
        });
        connect(m_autostartCheck, &QCheckBox::toggled, this, [this](bool on) {
            setAutostartEnabled(on);
            Q_EMIT autostartChanged(on);
        });
        connect(m_primaryOnlyCheck, &QCheckBox::toggled, this, [controller, this](bool on) {
            updateOutputRowState();
            if (!m_updatingFromEffect) {
                controller->requestPrimaryOnly(on);
            }
        });
        // activated() fires only for user choices — programmatic rebuilds
        // never push back to the effect.
        connect(m_primaryOutputCombo, &QComboBox::activated, this, [controller, this](int index) {
            controller->requestSetPrimaryOutput(m_primaryOutputCombo->itemData(index).toString());
        });
        connect(controller, &VibranceController::primaryOutputStateFromEffect, this,
                [controller, this](bool supported, const QString &name) {
            updateOutputRowState();
            if (supported) {
                controller->requestScreenList(name); // combo rebuilt on screenListFromEffect
            }
        });
        connect(controller, &VibranceController::screenListFromEffect, this,
                [this](const QString &name, const QStringList &screens) {
            {
                m_primaryOutputCombo->clear();
                m_primaryOutputCombo->addItem(tr("Automatic (first output)"), QString());
                for (const QString &entry : screens) {
                    const int sep = entry.indexOf(QLatin1Char('|'));
                    const QString outputName = sep < 0 ? entry : entry.left(sep);
                    const QString desc = sep < 0 ? QString() : entry.mid(sep + 1);
                    m_primaryOutputCombo->addItem(desc.isEmpty() ? outputName
                                                                 : QStringLiteral("%1 — %2").arg(outputName, desc),
                                                  outputName);
                }
                const int idx = m_primaryOutputCombo->findData(name);
                m_primaryOutputCombo->setCurrentIndex(idx < 0 ? 0 : idx);
            }
            updateOutputRowState();
        });
        connect(controller, &VibranceController::primaryOnlyFromEffect, this, [this](bool on) {
            m_updatingFromEffect = true;
            m_primaryOnlyCheck->setChecked(on);
            m_updatingFromEffect = false;
        });

        setCentralWidget(central);
        resize(800, 640);

        connect(m_slider, &QSlider::valueChanged, this, [controller, this](int percent) {
            updateValueLabel(percent);
            // Dragging to (or past) a preset must select/clear the segment.
            wvseg::syncPresetGroup(m_presetGroup, percent);
            if (!m_updatingFromEffect) {
                controller->requestVibrancePercent(percent);
            }
        });
        connect(m_enabledCheck, &QCheckBox::toggled, this, [controller, this](bool enabled) {
            if (!m_updatingFromEffect) {
                controller->requestVibranceEnabled(enabled);
            }
        });
        connect(controller, &VibranceController::vibranceFromEffect, this, [this](double value) {
            m_updatingFromEffect = true;
            setPercent(static_cast<int>(qRound(value * 100.0)));
            m_updatingFromEffect = false;
        });
        connect(controller, &VibranceController::enabledFromEffect, this, [this](bool enabled) {
            m_updatingFromEffect = true;
            m_enabledCheck->setChecked(enabled);
            m_updatingFromEffect = false;
        });
        connect(controller, &VibranceController::rgbGainsFromEffect, this, [this](double red, double green, double blue) {
            m_updatingRGB = true;
            setGains(static_cast<int>(qRound(red * 100.0)),
                     static_cast<int>(qRound(green * 100.0)),
                     static_cast<int>(qRound(blue * 100.0)));
            m_updatingRGB = false;
        });
        connect(controller, &VibranceController::availableChanged, this, [this](bool available) {
            m_statusLabel->setText(available ? tr("Connected to the Way Vibrance effect.")
                                             : tr("The effect is not loaded in KWin."));
            setEffectControlsEnabled(available);
            m_loadButton->setVisible(!available);
        });

        m_controller = controller;
        const bool available = controller->effectAvailable();
        m_statusLabel->setText(available
                                  ? tr("Connected to the Way Vibrance effect.")
                                  : tr("The effect is not loaded in KWin."));
        setEffectControlsEnabled(available);
        m_loadButton->setVisible(!available);

        // Sync every control with the effect's real state now that all of
        // our connections exist (earlier state pushes may have gone unheard).
        controller->refreshState();
    }

protected:
    bool event(QEvent *event) override
    {
        // Plasma can switch the colour scheme at runtime (day/night). Rebuild
        // the single application sheet instead of leaving cards, borders and
        // reset buttons with the palette they were created with.
        if (event->type() == QEvent::ApplicationPaletteChange && !m_applyingStyle) {
            m_applyingStyle = true;
            qApp->setStyleSheet(wvstyle::buildAppStyle(qApp->palette()));
            m_applyingStyle = false;
        }
        return QMainWindow::event(event);
    }

private:
    void updateProfileEmptyState()
    {
        if (!m_profileStack) {
            return;
        }
        m_profileStack->setCurrentIndex(m_profileList->count() == 0 ? 0 : 1);
    }

    void setEffectControlsEnabled(bool enabled)
    {
        m_slider->setEnabled(enabled);
        m_enabledCheck->setEnabled(enabled);
        m_redSlider->setEnabled(enabled);
        m_greenSlider->setEnabled(enabled);
        m_blueSlider->setEnabled(enabled);
        m_primaryOnlyCheck->setEnabled(enabled);
        m_primaryOutputRow->setEnabled(enabled && m_primaryOnlyCheck->isChecked());
        m_profileGroup->setEnabled(enabled);
    }

    void setPercent(int percent)
    {
        percent = qBound(kMinPercent, percent, kMaxPercent);
        m_slider->setValue(percent);
        updateValueLabel(percent);
        // Called both by preset clicks and by effect-state pushes. Syncing
        // here covers the "clicked the already-active preset" case, where
        // setValue() emits nothing at all.
        wvseg::syncPresetGroup(m_presetGroup, percent);
    }

    void setGains(int red, int green, int blue)
    {
        m_redSlider->setValue(qBound(kRgbMinPercent, red, kRgbMaxPercent));
        m_greenSlider->setValue(qBound(kRgbMinPercent, green, kRgbMaxPercent));
        m_blueSlider->setValue(qBound(kRgbMinPercent, blue, kRgbMaxPercent));
    }

    void pushRGBGains()
    {
        if (m_updatingRGB) {
            return;
        }
        m_controller->requestRGBGains(m_redSlider->value() / 100.0,
                                      m_greenSlider->value() / 100.0,
                                      m_blueSlider->value() / 100.0);
    }

    void updateValueLabel(int percent)
    {
        m_valueLabel->setText(QStringLiteral("%1%").arg(percent));
    }

    // The picker row stays put (no layout jump when the switch flips):
    // it is visible whenever the effect supports choosing the output, and
    // disabled — faded label and combo — while the mode itself is off.
    void updateOutputRowState()
    {
        const bool supported = m_controller && m_controller->primaryOutputSupported();
        m_primaryOutputRow->setVisible(supported);
        m_primaryOutputRow->setEnabled(supported && m_primaryOnlyCheck->isChecked());
    }

    void rebuildProfileList(const QStringList &entries)
    {
        if (entries == m_profileEntries) {
            return; // echo of what we already show: keep the user's place
        }
        const int previousRow = m_profileList->currentRow();
        m_profileEntries = entries;
        m_profileList->clear();
        for (const QString &entry : entries) {
            ProfileValues values;
            if (!parseProfileEntry(entry, values)) {
                continue;
            }
            // The custom display name wins over the derived friendly one;
            // it is cosmetic — matching always keeps using the pattern.
            QString text = QStringLiteral("%1 — %2%")
                               .arg(values.displayName.isEmpty() ? friendlyAppName(values.prefixedPattern)
                                                                 : values.displayName)
                               .arg(values.vibrancePercent);
            if (values.hasGains) {
                text += QStringLiteral("  [+RGB %1/%2/%3]")
                            .arg(values.redPercent)
                            .arg(values.greenPercent)
                            .arg(values.bluePercent);
            }
            auto *item = new QListWidgetItem(text, m_profileList);
            // Rows are bound to their own serialized entry: unparseable
            // entries are skipped above, so indexing m_profileEntries by row
            // would shift every later profile once one is rejected.
            item->setData(Qt::UserRole, entry);
            item->setToolTip(QStringLiteral("%1\n%2").arg(values.prefixedPattern, entry));
        }
        if (previousRow >= 0 && previousRow < m_profileList->count()) {
            m_profileList->setCurrentRow(previousRow);
        }
        updateProfileEmptyState();
    }

    QString currentProfilePattern() const
    {
        const QListWidgetItem *item = m_profileList->currentItem();
        if (!item) {
            return QString();
        }
        ProfileValues values;
        if (!parseProfileEntry(item->data(Qt::UserRole).toString(), values)) {
            return QString();
        }
        return values.prefixedPattern;
    }

    VibranceController *m_controller = nullptr;
    QSlider *m_slider = nullptr;
    QSlider *m_redSlider = nullptr;
    QSlider *m_greenSlider = nullptr;
    QSlider *m_blueSlider = nullptr;
    QGroupBox *m_profileGroup = nullptr;
    QButtonGroup *m_presetGroup = nullptr;
    QWidget *m_sidePane = nullptr;
    QStackedWidget *m_profileStack = nullptr;
    QListWidget *m_profileList = nullptr;
    QStringList m_profileEntries;

public:
    void setAutostartChecked(bool on)
    {
        if (m_autostartCheck) {
            m_autostartCheck->setChecked(on);
        }
    }

    void showAbout()
    {
        auto *dialog = new QDialog(this);
        dialog->setWindowTitle(tr("About Way Vibrance"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModal(true);

        auto *layout = new QVBoxLayout(dialog);
        layout->setSpacing(12);

        auto *title = new QLabel(QStringLiteral("<b>Way Vibrance</b> %1").arg(
                                     QStringLiteral(WAVERBANCE_VERSION)),
                                 dialog);
        title->setTextFormat(Qt::RichText);
        QFont titleFont = title->font();
        // Style sheets may define fonts in pixels (pointSizeF() == -1); only
        // scale what actually exists so we never pass a negative size.
        if (titleFont.pointSizeF() > 0.0) {
            titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
        } else if (titleFont.pixelSize() > 0) {
            titleFont.setPixelSize(qRound(titleFont.pixelSize() * 1.3));
        }
        title->setFont(titleFont);
        layout->addWidget(title);

        auto *description = new QLabel(
            tr("System-wide vibrance and per-channel RGB control for KDE "
               "Plasma on Wayland.<br><br>"
               "A KWin effect plus a tray controller, talking to each other "
               "over D-Bus for flicker-free live adjustments.<br><br>"
               "<a href=\"https://github.com/Machad3x/way-vibrance\">"
               "github.com/Machad3x/way-vibrance</a>"),
            dialog);
        description->setWordWrap(true);
        description->setTextFormat(Qt::RichText);
        description->setOpenExternalLinks(true);
        layout->addWidget(description);

        auto *license = new QLabel(tr("Licensed under the GPL-2.0-or-later."), dialog);
        license->setProperty("wvHint", true);
        layout->addWidget(license);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        layout->addWidget(buttons);

        dialog->open();
    }

    void checkForUpdates()
    {
        const QString currentVersion = QStringLiteral(WAVERBANCE_VERSION);

        auto *dialog = new QDialog(this);
        dialog->setWindowTitle(tr("Check for Updates"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModal(true);
        dialog->setMinimumWidth(360);

        auto *layout = new QVBoxLayout(dialog);
        layout->setSpacing(10);

        auto *status = new QLabel(tr("Contacting GitHub…"), dialog);
        status->setWordWrap(true);
        status->setTextFormat(Qt::RichText);
        status->setOpenExternalLinks(true);
        layout->addWidget(status);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        layout->addWidget(buttons);

        // The manager is parented to the dialog, so both die together if the
        // user closes the dialog mid-request.
        auto *manager = new QNetworkAccessManager(dialog);
        QNetworkRequest request(
            QUrl(QStringLiteral("https://api.github.com/repos/Machad3x/way-vibrance/releases/latest")));
        request.setRawHeader("User-Agent", "WayVibrance/" WAVERBANCE_VERSION);
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);

        QNetworkReply *reply = manager->get(request);
        connect(reply, &QNetworkReply::finished, dialog, [reply, status, currentVersion]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                status->setText(tr("Could not reach GitHub: %1")
                                    .arg(reply->errorString().toHtmlEscaped()));
                return;
            }

            QJsonParseError parseError{};
            const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                status->setText(tr("GitHub returned an unexpected response."));
                return;
            }

            const QJsonObject release = doc.object();
            const QString latestVersion = release.value(QLatin1String("tag_name")).toString();
            QString releaseUrl = release.value(QLatin1String("html_url")).toString();
            if (releaseUrl.isEmpty()) {
                releaseUrl = QStringLiteral("https://github.com/Machad3x/way-vibrance/releases/latest");
            }
            if (latestVersion.isEmpty()) {
                status->setText(tr("GitHub did not report a release tag."));
                return;
            }

            if (wvversion::compare(latestVersion, currentVersion) <= 0) {
                status->setText(tr("You are running the latest version (%1).")
                                    .arg(currentVersion));
                return;
            }

            status->setText(
                tr("<b>A newer version is available: %1</b><br><br>"
                   "You are running %2.<br><br>"
                   "%3<br><br>"
                   "Alternatively, download the package for your system "
                   "from the release page:<br>"
                   "<a href=\"%4\">%4</a>")
                    .arg(latestVersion.toHtmlEscaped(),
                         currentVersion,
                         installMethodHint(),
                         releaseUrl.toHtmlEscaped()));
        });

        dialog->open();
    }

Q_SIGNALS:
    void autostartChanged(bool enabled);

private:
    QLabel *m_valueLabel = nullptr;
    QLabel *m_redLabel = nullptr;
    QLabel *m_greenLabel = nullptr;
    QLabel *m_blueLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    wvswitch::Switch *m_enabledCheck = nullptr;
    wvswitch::Switch *m_startHiddenCheck = nullptr;
    wvswitch::Switch *m_autostartCheck = nullptr;
    wvswitch::Switch *m_primaryOnlyCheck = nullptr;
    QWidget *m_primaryOutputRow = nullptr;
    QComboBox *m_primaryOutputCombo = nullptr;
    QPushButton *m_loadButton = nullptr;
    QKeySequenceEdit *m_shortcutEdit = nullptr;
    QLabel *m_shortcutInfo = nullptr;
    bool m_updatingFromEffect = false;
    bool m_updatingRGB = false;
    bool m_updatingShortcut = false;
    bool m_applyingStyle = false;
};

static QString autostartFilePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("autostart/org.kde.wayvibrance-tray.desktop"));
}

static QString legacyAutostartFilePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("autostart/wayvibrance.desktop"));
}

static void setAutostartEnabled(bool enabled);

static bool autostartEnabled()
{
    // Migrate the entry from the old file name (and the old icon-less stub)
    // on the fly: any legacy file means "enabled", and we regenerate a
    // complete entry under the new name.
    if (QFile::exists(autostartFilePath())) {
        return true;
    }
    if (QFile::exists(legacyAutostartFilePath())) {
        QFile::remove(legacyAutostartFilePath());
        setAutostartEnabled(true);
        return true;
    }
    return false;
}

static void setAutostartEnabled(bool enabled)
{
    const QString target = autostartFilePath();
    if (!enabled) {
        QFile::remove(target);
        QFile::remove(legacyAutostartFilePath());
        return;
    }
    QDir().mkpath(QFileInfo(target).absolutePath());

    // Prefer the installed desktop entry: Plasma's Autostart settings then
    // show the full record — name, icon, comment — like any other app.
    const QString installed = QStandardPaths::locate(
        QStandardPaths::ApplicationsLocation, QStringLiteral("org.kde.wayvibrance-tray.desktop"));
    QFile::remove(target);
    if (!installed.isEmpty() && QFile::copy(installed, target)) {
        return;
    }

    // Fallback (running straight from the build tree, app file missing).
    QFile file(target);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write("[Desktop Entry]\n"
                   "Type=Application\n"
                   "Name=Way Vibrance\n"
                   "Exec=wayvibrance-tray\n"
                   "Icon=preferences-desktop-color\n"
                   "X-GNOME-Autostart-enabled=true\n");
    }
}

// "Open directly to the tray": when true, launching the app does not show
// the window; the tray icon (left click) is the entry point.
static bool storedStartHidden()
{
    auto config = KSharedConfig::openConfig(QStringLiteral("wayvibrancerc"));
    return config->group(QStringLiteral("General")).readEntry("StartHidden", false);
}

static void saveStartHidden(bool hidden)
{
    auto config = KSharedConfig::openConfig(QStringLiteral("wayvibrancerc"));
    KConfigGroup group(config, QStringLiteral("General"));
    group.writeEntry("StartHidden", hidden);
    config->sync();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // One consistent identity everywhere (D-Bus service, Wayland app_id,
    // portal lookup, KGlobalAccel component): org.kde.wayvibrance-tray.
    QApplication::setApplicationName(QStringLiteral("org.kde.wayvibrance-tray"));
    QApplication::setOrganizationName(QStringLiteral("wayvibrance"));
    QApplication::setApplicationDisplayName(QStringLiteral("Way Vibrance"));
    QApplication::setQuitOnLastWindowClosed(false);

    // One palette-derived sheet for the tray window and the profile dialog.
    // Applied before any widget exists so the first polish is already themed.
    qApp->setStyleSheet(wvstyle::buildAppStyle(qApp->palette()));

    KAboutData about(QStringLiteral("org.kde.wayvibrance-tray"),
                     QStringLiteral("Way Vibrance"),
                     QStringLiteral(WAVERBANCE_VERSION),
                     QObject::tr("System-wide vibrance and per-channel RGB control for KDE Plasma on Wayland"),
                     KAboutLicense::GPL);
    about.setDesktopFileName(QStringLiteral("org.kde.wayvibrance-tray"));
    KAboutData::setApplicationData(about);

    // Single instance + D-Bus registration under the component name, which
    // is how the KGlobalAccel daemon reaches us when the hotkey fires.
    KDBusService dbusService(KDBusService::Unique);

    VibranceController controller;

    // Our config (loaded by the controller constructor) is the source of
    // truth. Wipe any stale daemon state for our action and (re)apply it
    // once the event loop is running — before that, the Component D-Bus
    // adaptor is not served yet and the daemon answers nothing useful.
    QTimer::singleShot(0, &controller, &VibranceController::applyStartupShortcut);

    TrayWindow window(&controller);

    auto *icon = new QSystemTrayIcon(QIcon::fromTheme(QStringLiteral("preferences-desktop-color")), &app);
    auto *menu = new QMenu(&window);

    auto *openAction = menu->addAction(QIcon::fromTheme(QStringLiteral("preferences-desktop-color")),
                                       QObject::tr("Open Way Vibrance…"));
    QObject::connect(openAction, &QAction::triggered, [&window] {
        window.show();
        window.raise();
        window.activateWindow();
    });

    menu->addSeparator();
    auto *enabledAction = menu->addAction(QObject::tr("Enabled"));
    enabledAction->setCheckable(true);
    enabledAction->setChecked(true);
    // Guard: ignore toggles while the effect attachment is in flight to
    // prevent race conditions in the D-Bus service registration/unregistration.
    QObject::connect(enabledAction, &QAction::toggled, &controller, [&controller](bool enabled) {
        if (!controller.effectAvailable()) return;
        controller.requestVibranceEnabled(enabled);
    });
    QObject::connect(&controller, &VibranceController::enabledFromEffect, enabledAction, &QAction::setChecked);
    QObject::connect(&controller, &VibranceController::availableChanged, enabledAction,
                     [enabledAction](bool available) { enabledAction->setEnabled(available); });
    enabledAction->setEnabled(controller.effectAvailable());

    menu->addSeparator();
    for (int percent : wvseg::kPresetValues) {
        auto *action = menu->addAction(QObject::tr("Set %1%").arg(percent));
        QObject::connect(action, &QAction::triggered, &controller, [percent, &controller] {
            controller.ensureEffectLoaded();
            controller.requestVibrancePercent(percent);
        });
    }

    menu->addSeparator();
    auto *autostartAction = menu->addAction(QObject::tr("Start with Plasma"));
    autostartAction->setCheckable(true);
    autostartAction->setChecked(autostartEnabled());
    QObject::connect(autostartAction, &QAction::toggled, [&window](bool enabled) {
        setAutostartEnabled(enabled);
        window.setAutostartChecked(enabled); // keep the window checkbox in sync
    });
    QObject::connect(&window, &TrayWindow::autostartChanged, autostartAction, &QAction::setChecked);

    menu->addSeparator();
    auto *aboutAction = menu->addAction(QIcon::fromTheme(QStringLiteral("help-about")),
                                        QObject::tr("About Way Vibrance"));
    QObject::connect(aboutAction, &QAction::triggered, &window, &TrayWindow::showAbout);
    auto *updateAction = menu->addAction(QIcon::fromTheme(QStringLiteral("system-software-update")),
                                         QObject::tr("Check for Updates…"));
    QObject::connect(updateAction, &QAction::triggered, &window, &TrayWindow::checkForUpdates);

    menu->addSeparator();
    auto *quitAction = menu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")), QObject::tr("Quit"));
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);

    QObject::connect(&controller, &VibranceController::vibranceFromEffect, icon, [icon](double value) {
        icon->setToolTip(QStringLiteral("Way Vibrance — %1%")
                             .arg(static_cast<int>(qRound(value * 100.0))));
    });

    icon->setContextMenu(menu);
    icon->setToolTip(QStringLiteral("Way Vibrance"));
    icon->show();
    QObject::connect(icon, &QSystemTrayIcon::activated, &window, [&window](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            window.setVisible(!window.isVisible());
        }
    });

    // Match vibrance-gui's core loop: make sure the effect is in KWin's chain.
    controller.ensureEffectLoaded();

    // Second full state sync once the event loop starts: by then the effect
    // is loaded and the startup shortcut applied, and every receiver —
    // including the tray menu actions and the tooltip — is connected.
    QTimer::singleShot(400, &controller, &VibranceController::refreshState);

    // Show the window on launch unless the user asked to open straight to
    // the tray ("Behaviour → Open directly to the tray on startup").
    if (!storedStartHidden()) {
        window.show();
    }

    return app.exec();
}

#include "main.moc"
