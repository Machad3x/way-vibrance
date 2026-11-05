/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Way Vibrance tray controller: the Linux/Wayland (KDE) counterpart of
// vibrance-gui. Talks to the "wayvibrance" KWin effect over D-Bus for
// flicker-free live adjustments.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QAction>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include <KAboutData>
#include <KConfigGroup>
#include <KDBusService>
#include <KGlobalAccel>
#include <KGlobalShortcutInfo>
#include <KSharedConfig>

static constexpr auto s_effectId = "wayvibrance";
static constexpr auto s_vibranceService = "org.wayvibrance";
static constexpr auto s_vibrancePath = "/Vibrance";
static constexpr auto s_vibranceInterface = "org.wayvibrance";
static constexpr auto s_toggleActionId = "toggle-vibrance";
static constexpr int kMinPercent = 0;
static constexpr int kMaxPercent = 200;
static constexpr int kRgbMinPercent = 0;
static constexpr int kRgbMaxPercent = 200;

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

        // The effect may already be running (loaded by a previous session's
        // configuration); connect to it right away.
        const QDBusReply<bool> registered =
            QDBusConnection::sessionBus().interface()->isServiceRegistered(QLatin1String(s_vibranceService));
        if (registered.isValid() && registered.value()) {
            attachInterface();
        }
    }

    bool effectAvailable() const { return m_interface != nullptr; }

    /** The QAction backing the configurable global hotkey (toggle on/off). */
    QAction *toggleAction() const { return m_toggleAction; }

    /** Loads the effect into KWin if it is not already running. */
    void ensureEffectLoaded()
    {
        // KWin 6.8+ hosts the /Effects object under the org.kde.KWin service;
        // older setups also exported a dedicated org.kde.kwin.Effects service.
        // Try both, same interface.
        static constexpr const char *kServices[] = {"org.kde.KWin", "org.kde.kwin.Effects"};
        for (const char *service : kServices) {
            QDBusInterface effects(QLatin1String(service),
                                   QStringLiteral("/Effects"),
                                   QStringLiteral("org.kde.kwin.Effects"));
            if (!effects.isValid()) {
                continue;
            }
            const QDBusReply<bool> loaded = effects.call(QStringLiteral("isEffectLoaded"),
                                                         QString::fromLatin1(s_effectId));
            if (loaded.isValid() && loaded.value()) {
                return;
            }
            // Fire-and-forget: when the effect comes up it registers
            // org.wayvibrance and the service watcher attaches to it.
            effects.asyncCall(QStringLiteral("loadEffect"), QString::fromLatin1(s_effectId));
            return;
        }
    }

    void requestVibrancePercent(int percent)
    {
        m_pendingPercent = percent;
        m_timer.start();
    }

    void requestVibranceEnabled(bool enabled)
    {
        if (m_interface) {
            m_interface->asyncCall(QStringLiteral("setEnabled"), enabled);
        }
    }

    void requestPrimaryOnly(bool primaryOnly)
    {
        if (m_interface) {
            m_interface->asyncCall(QStringLiteral("setPrimaryOnly"), primaryOnly);
            QTimer::singleShot(160, this, &VibranceController::refreshState);
        }
    }

    /**
     * Pulls the full current state from the effect and re-emits it as if it
     * had changed remotely, so every slider/checkbox is truthful even if a
     * D-Bus signal was ever missed (and on startup, before any change).
     */
    void refreshState()
    {
        if (!m_interface) {
            return;
        }
        const QDBusReply<double> value = m_interface->call(QStringLiteral("vibrance"));
        if (value.isValid()) {
            onVibranceChanged(value.value());
        }
        const QDBusReply<bool> enabled = m_interface->call(QStringLiteral("enabled"));
        if (enabled.isValid()) {
            onEnabledChanged(enabled.value());
        }
        const QDBusReply<bool> primary = m_interface->call(QStringLiteral("primaryOnly"));
        if (primary.isValid()) {
            onPrimaryOnlyChanged(primary.value());
        }
        const QDBusReply<double> red = m_interface->call(QStringLiteral("redGain"));
        const QDBusReply<double> green = m_interface->call(QStringLiteral("greenGain"));
        const QDBusReply<double> blue = m_interface->call(QStringLiteral("blueGain"));
        if (red.isValid() && green.isValid() && blue.isValid()) {
            onRGBGainsChanged(red.value(), green.value(), blue.value());
        }
        // Per-app profiles: an effect older than this feature answers
        // UnknownMethod — hide the UI instead of pretending the rules apply.
        const QDBusReply<QStringList> rules = m_interface->call(QStringLiteral("appRules"));
        setProfilesSupported(rules.isValid());
        if (rules.isValid()) {
            onAppRulesChanged(rules.value());
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

    /** Creates or updates a per-app profile; the effect echoes the full list. */
    void requestSetAppRule(const QString &prefixedPattern, double vibrance)
    {
        if (m_interface) {
            m_interface->asyncCall(QStringLiteral("setAppRule"), prefixedPattern, vibrance);
            // Do not depend on the echo alone: pull the authoritative state.
            QTimer::singleShot(120, this, &VibranceController::refreshState);
        }
    }

    void requestSetAppRuleFull(const QString &prefixedPattern, double vibrance,
                               double red, double green, double blue)
    {
        if (m_interface) {
            m_interface->asyncCall(QStringLiteral("setAppRuleFull"), prefixedPattern, vibrance,
                                   red, green, blue);
            QTimer::singleShot(120, this, &VibranceController::refreshState);
        }
    }

    void requestRemoveAppRule(const QString &prefixedPattern)
    {
        if (m_interface) {
            m_interface->asyncCall(QStringLiteral("removeAppRule"), prefixedPattern);
            QTimer::singleShot(120, this, &VibranceController::refreshState);
        }
    }

    /** Resource class (Wayland app_id / X11 WM_CLASS) of the last focused app. */
    QString fetchLastFocusedAppId()
    {
        if (!m_interface) {
            return QString();
        }
        const QDBusReply<QString> reply = m_interface->call(QStringLiteral("lastFocusedAppId"));
        return reply.isValid() ? reply.value() : QString();
    }

    bool profilesSupported() const { return m_profilesSupported; }

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
        const QList<QVariant> args = msg.arguments();
        const QString member = msg.member();
        if (member == QLatin1String("vibranceChanged") && args.size() == 1) {
            onVibranceChanged(args.at(0).toDouble());
        } else if (member == QLatin1String("enabledChanged") && args.size() == 1) {
            onEnabledChanged(args.at(0).toBool());
        } else if (member == QLatin1String("rgbGainsChanged") && args.size() == 3) {
            onRGBGainsChanged(args.at(0).toDouble(), args.at(1).toDouble(), args.at(2).toDouble());
        } else if (member == QLatin1String("primaryOnlyChanged") && args.size() == 1) {
            onPrimaryOnlyChanged(args.at(0).toBool());
        } else if (member == QLatin1String("appRulesChanged") && args.size() == 1) {
            onAppRulesChanged(args.at(0).toStringList());
        }
    }

    void onVibranceChanged(double value)
    {
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

    /** What the global hotkey runs: an atomic, server-side flip. */
    void toggleVibrance()
    {
        if (!m_interface) {
            // Not connected yet: bring the effect up; the next press toggles.
            ensureEffectLoaded();
            return;
        }
        // The effect flips its own state — no cached copy to go stale.
        m_interface->asyncCall(QStringLiteral("toggleEnabled"));
        // Belt and braces: re-pull state shortly after so every checkbox and
        // the tooltip reflect reality even if the signal path misbehaves.
        QTimer::singleShot(160, this, &VibranceController::refreshState);
    }

private:
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
        if (!m_interface || !m_rgbPending) {
            return;
        }
        m_rgbPending = false;
        m_interface->asyncCall(QStringLiteral("setRGBGains"), m_pendingRed, m_pendingGreen, m_pendingBlue);
    }
    void flush()
    {
        if (!m_interface || m_pendingPercent < 0) {
            return;
        }
        const double value = m_pendingPercent / 100.0;
        m_pendingPercent = -1;
        if (qFuzzyCompare(value, m_lastSent)) {
            return;
        }
        m_lastSent = value;
        m_interface->asyncCall(QStringLiteral("setVibrance"), value);
    }

    void attachInterface()
    {
        if (m_interface) {
            return;
        }
        m_interface = new QDBusInterface(QString::fromLatin1(s_vibranceService),
                                         QString::fromLatin1(s_vibrancePath),
                                         QString::fromLatin1(s_vibranceInterface),
                                         QDBusConnection::sessionBus(),
                                         this);

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

        refreshState();
        Q_EMIT availableChanged(true);
    }

    void detachInterface()
    {
        if (!m_interface) {
            return;
        }
        auto bus = QDBusConnection::sessionBus();
        bus.disconnect(QString(),
                       QString::fromLatin1(s_vibrancePath),
                       QString::fromLatin1(s_vibranceInterface),
                       QString(),
                       this,
                       SLOT(onSignal(QDBusMessage)));
        delete m_interface;
        m_interface = nullptr;
        m_lastSent = -1.0;
        setProfilesSupported(false);
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

    QDBusInterface *m_interface = nullptr;
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
    int vibrancePercent = 100;
    int redPercent = 100;
    int greenPercent = 100;
    int bluePercent = 100;
    bool hasGains = false;
};

static bool parseProfileEntry(const QString &entry, ProfileValues &out)
{
    // gains? last field "r,g,b"
    const int lastSep = entry.lastIndexOf(QLatin1Char('|'));
    if (lastSep <= 0) {
        return false;
    }
    const QString tail = entry.mid(lastSep + 1);
    QString rest = entry.left(lastSep);
    bool ok = false;
    double vib = 1.0;
    int red = 100, green = 100, blue = 100;
    bool hasGains = false;
    const QStringList gainsParts = tail.split(QLatin1Char(','));
    if (gainsParts.size() == 3) {
        const double r = gainsParts.at(0).toDouble(&ok);
        if (!ok) {
            return false;
        }
        const double g = gainsParts.at(1).toDouble(&ok);
        if (!ok) {
            return false;
        }
        const double b = gainsParts.at(2).toDouble(&ok);
        if (!ok) {
            return false;
        }
        red = qRound(r * 100.0);
        green = qRound(g * 100.0);
        blue = qRound(b * 100.0);
        hasGains = true;
        const int vibSep = rest.lastIndexOf(QLatin1Char('|'));
        if (vibSep <= 0) {
            return false;
        }
        vib = rest.mid(vibSep + 1).toDouble(&ok);
        if (!ok) {
            return false;
        }
        rest = rest.left(vibSep);
    } else {
        vib = tail.toDouble(&ok);
        if (!ok) {
            return false;
        }
    }
    out.prefixedPattern = rest;
    out.vibrancePercent = qBound(0, static_cast<int>(qRound(vib * 100.0)), 200);
    out.redPercent = qBound(0, red, 200);
    out.greenPercent = qBound(0, green, 200);
    out.bluePercent = qBound(0, blue, 200);
    out.hasGains = hasGains;
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
 * Add/edit dialog for a per-app profile: match kind, glob pattern and the
 * vibrance that matching windows should use instead of the global value.
 */
class ProfileDialog : public QDialog
{
public:
    explicit ProfileDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(tr("Per-app profile"));
        auto *layout = new QVBoxLayout(this);

        auto *matchRow = new QHBoxLayout();
        m_kind = new QComboBox(this);
        m_kind->addItem(tr("App ID / class"));
        m_kind->addItem(tr("Window title"));
        m_pattern = new QLineEdit(this);
        m_pattern->setPlaceholderText(QStringLiteral("firefox, *steam*, org.kde.dolphin…"));
        matchRow->addWidget(m_kind);
        matchRow->addWidget(m_pattern, 1);
        layout->addLayout(matchRow);

        auto *valueRow = new QHBoxLayout();
        valueRow->addWidget(new QLabel(tr("Vibrance"), this));
        m_percent = new QSpinBox(this);
        m_percent->setRange(kMinPercent, kMaxPercent);
        m_percent->setValue(125);
        m_percent->setSuffix(QStringLiteral("%"));
        valueRow->addWidget(m_percent, 1);
        layout->addLayout(valueRow);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        m_okButton = buttons->button(QDialogButtonBox::Ok);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_pattern, &QLineEdit::textChanged, this, [this] { validate(); });
        connect(m_kind, &QComboBox::currentIndexChanged, this, [this] { validate(); });

        resize(380, 120);
        validate();
    }

    void setFields(bool titleKind, const QString &pattern, int percent)
    {
        m_kind->setCurrentIndex(titleKind ? 1 : 0);
        m_pattern->setText(pattern);
        m_percent->setValue(percent);
        validate();
    }

    /** The pattern with its kind prefix, as the effect's setAppRule takes it. */
    QString prefixedPattern() const
    {
        const QString body = m_pattern->text().trimmed();
        return (m_kind->currentIndex() == 1 ? QStringLiteral("title:") : QStringLiteral("app:")) + body;
    }

    double ruleVibrance() const { return m_percent->value() / 100.0; }

private:
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
    QSpinBox *m_percent = nullptr;
    QPushButton *m_okButton = nullptr;
};

class TrayWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit TrayWindow(VibranceController *controller, QWidget *parent = nullptr)
        : QMainWindow(parent)
    {
        setWindowTitle(tr("Way Vibrance"));

        auto *central = new QWidget(this);
        // Two panes: the global controls on the left, the per-app profile
        // editor on the right — a distinct "section" of the app that hides
        // (and lets the window collapse back) when unsupported.
        auto *columns = new QHBoxLayout(central);
        auto *outerLayout = new QVBoxLayout();
        columns->addLayout(outerLayout, 5);
        m_sidePane = new QWidget(central);
        auto *sideLayout = new QVBoxLayout(m_sidePane);
        sideLayout->setContentsMargins(0, 0, 0, 0);
        columns->addWidget(m_sidePane, 4);

        m_statusLabel = new QLabel(central);
        outerLayout->addWidget(m_statusLabel);

        auto *row = new QHBoxLayout();
        m_slider = new QSlider(Qt::Horizontal, central);
        m_slider->setRange(kMinPercent, kMaxPercent);
        m_slider->setValue(100);
        m_valueLabel = new QLabel(central);
        m_valueLabel->setMinimumWidth(48);
        row->addWidget(m_slider);
        row->addWidget(m_valueLabel);
        outerLayout->addLayout(row);

        m_enabledCheck = new QCheckBox(tr("Enabled"), central);
        m_enabledCheck->setChecked(true);
        outerLayout->addWidget(m_enabledCheck);

        // Per-channel RGB gains (like the driver's color page). Percent
        // values map to gain multipliers (100% = neutral 1.0).
        auto *rgbGroup = new QGroupBox(tr("Channel gains (RGB)"), central);
        auto *rgbLayout = new QVBoxLayout(rgbGroup);
        struct ChannelSpec
        {
            const char *name;
            const char *color;
            QSlider *TrayWindow::*member;
            QLabel *TrayWindow::*label;
        };
        const ChannelSpec channels[] = {
            {"R", "#e05555", &TrayWindow::m_redSlider, &TrayWindow::m_redLabel},
            {"G", "#55c25a", &TrayWindow::m_greenSlider, &TrayWindow::m_greenLabel},
            {"B", "#5b8def", &TrayWindow::m_blueSlider, &TrayWindow::m_blueLabel},
        };
        for (const ChannelSpec &ch : channels) {
            auto *row = new QHBoxLayout();
            auto *name = new QLabel(QString::fromLatin1(ch.name), rgbGroup);
            name->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(QLatin1String(ch.color)));
            auto *slider = new QSlider(Qt::Horizontal, rgbGroup);
            slider->setRange(kRgbMinPercent, kMaxPercent);
            slider->setValue(100);
            auto *value = new QLabel(rgbGroup);
            value->setMinimumWidth(48);
            value->setText(QStringLiteral("100%"));
            row->addWidget(name);
            row->addWidget(slider);
            row->addWidget(value);
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
            setGains(100, 100, 100);
            if (m_controller) {
                m_controller->requestRGBGains(1.0, 1.0, 1.0);
            }
        });
        rgbLayout->addWidget(rgbReset);
        outerLayout->addWidget(rgbGroup);

        auto *presets = new QHBoxLayout();
        for (int percent : {100, 125, 150, 200}) {
            auto *button = new QPushButton(QStringLiteral("%1%").arg(percent), central);
            connect(button, &QPushButton::clicked, this, [this, percent] {
                setPercent(percent);
                if (m_controller) {
                    m_controller->requestVibrancePercent(percent);
                }
            });
            presets->addWidget(button);
        }
        presets->addStretch();
        outerLayout->addLayout(presets);

        // Per-app profiles (the counterpart of vibrance-gui's per-game
        // settings): matching windows override the global vibrance — and can
        // carry their own per-channel RGB gains, edited right here.
        m_profileGroup = new QGroupBox(tr("Per-app profiles"), m_sidePane);
        auto *profileLayout = new QVBoxLayout(m_profileGroup);
        m_profileList = new QListWidget(m_profileGroup);
        m_profileList->setMinimumWidth(280);
        m_profileList->setMaximumHeight(130);
        m_profileList->setToolTip(tr("Double-click a profile to edit its pattern; select one to edit its values below."));
        profileLayout->addWidget(m_profileList);

        m_profilePushTimer = new QTimer(this);
        m_profilePushTimer->setSingleShot(true);
        m_profilePushTimer->setInterval(120);
        connect(m_profilePushTimer, &QTimer::timeout, this, [this] { pushProfileEdits(); });

        m_ruleEditor = new QGroupBox(tr("Profile settings"), m_profileGroup);
        auto *editorLayout = new QVBoxLayout(m_ruleEditor);
        m_ruleNameLabel = new QLabel(m_ruleEditor);
        m_ruleNameLabel->setTextFormat(Qt::PlainText);
        editorLayout->addWidget(m_ruleNameLabel);
        auto *editorHint = new QLabel(tr("Vibrance of matching windows"), m_ruleEditor);
        editorLayout->addWidget(editorHint);
        auto *vibRow = new QHBoxLayout();
        m_ruleVibSlider = new QSlider(Qt::Horizontal, m_ruleEditor);
        m_ruleVibSlider->setRange(kMinPercent, kMaxPercent);
        m_ruleVibValue = new QLabel(m_ruleEditor);
        m_ruleVibValue->setMinimumWidth(44);
        vibRow->addWidget(m_ruleVibSlider);
        vibRow->addWidget(m_ruleVibValue);
        editorLayout->addLayout(vibRow);
        auto *gainsLabel = new QLabel(tr("Channel gains (this window only)"), m_ruleEditor);
        editorLayout->addWidget(gainsLabel);
        struct RuleChannelSpec
        {
            const char *name;
            const char *color;
            QSlider *TrayWindow::*slider;
            QLabel *TrayWindow::*value;
        };
        const RuleChannelSpec ruleChannels[] = {
            {"R", "#e05555", &TrayWindow::m_ruleRedSlider, &TrayWindow::m_ruleRedValue},
            {"G", "#55c25a", &TrayWindow::m_ruleGreenSlider, &TrayWindow::m_ruleGreenValue},
            {"B", "#5b8def", &TrayWindow::m_ruleBlueSlider, &TrayWindow::m_ruleBlueValue},
        };
        for (const RuleChannelSpec &ch : ruleChannels) {
            auto *row2 = new QHBoxLayout();
            auto *name = new QLabel(QString::fromLatin1(ch.name), m_ruleEditor);
            name->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(QLatin1String(ch.color)));
            name->setFixedWidth(12);
            auto *slider = new QSlider(Qt::Horizontal, m_ruleEditor);
            slider->setRange(kRgbMinPercent, kRgbMaxPercent);
            auto *value = new QLabel(m_ruleEditor);
            value->setMinimumWidth(44);
            value->setText(QStringLiteral("100%"));
            row2->addWidget(name);
            row2->addWidget(slider);
            row2->addWidget(value);
            editorLayout->addLayout(row2);
            this->*(ch.slider) = slider;
            this->*(ch.value) = value;
            connect(slider, &QSlider::valueChanged, this, [this, value](int percent) {
                value->setText(QStringLiteral("%1%").arg(percent));
                if (!m_loadingProfile) {
                    m_profilePushTimer->start();
                }
            });
        }
        auto *ruleResetRgb = new QPushButton(tr("Reset RGB"), m_ruleEditor);
        connect(ruleResetRgb, &QPushButton::clicked, this, [this] {
            m_ruleRedSlider->setValue(100);
            m_ruleGreenSlider->setValue(100);
            m_ruleBlueSlider->setValue(100);
            // 100/100/100 makes the profile drop back to the global gains
            // (and releases its dedicated shader on the effect side).
            if (!m_editingPattern.isEmpty()) {
                pushProfileEdits();
            }
        });
        editorLayout->addWidget(ruleResetRgb);
        m_ruleEditor->setVisible(false);
        profileLayout->addWidget(m_ruleEditor);

        auto *profileButtons = new QHBoxLayout();
        auto *addFocusedButton = new QPushButton(tr("From last focused…"), m_profileGroup);
        addFocusedButton->setToolTip(tr("Pre-fill the app ID of the window you focused before this one."));
        auto *addButton = new QPushButton(tr("Add…"), m_profileGroup);
        auto *removeButton = new QPushButton(tr("Remove"), m_profileGroup);
        profileButtons->addWidget(addFocusedButton);
        profileButtons->addWidget(addButton);
        profileButtons->addWidget(removeButton);
        profileButtons->addStretch();
        profileLayout->addLayout(profileButtons);
        auto *profileHint = new QLabel(tr("Windows whose app ID (Wayland app_id / X11 WM_CLASS) or title matches a profile are composited with its vibrance (and its own RGB gains, if set); '*' and '?' are wildcards."), m_profileGroup);
        profileHint->setWordWrap(true);
        profileLayout->addWidget(profileHint);
        profileLayout->addStretch(1);
        sideLayout->addWidget(m_profileGroup, 1);

        connect(addButton, &QPushButton::clicked, this, [controller, this] {
            ProfileDialog dialog(this);
            if (dialog.exec() == QDialog::Accepted) {
                controller->requestSetAppRule(dialog.prefixedPattern(), dialog.ruleVibrance());
            }
        });
        connect(addFocusedButton, &QPushButton::clicked, this, [controller, this] {
            const QString id = controller->fetchLastFocusedAppId();
            ProfileDialog dialog(this);
            if (!id.isEmpty()) {
                dialog.setFields(false, id, 125);
            }
            if (dialog.exec() == QDialog::Accepted) {
                controller->requestSetAppRule(dialog.prefixedPattern(), dialog.ruleVibrance());
            }
        });
        connect(removeButton, &QPushButton::clicked, this, [controller, this] {
            const QString pattern = currentProfilePattern();
            if (!pattern.isEmpty()) {
                controller->requestRemoveAppRule(pattern);
            }
        });
        connect(m_profileList, &QListWidget::currentRowChanged, this, [this](int row) {
            loadProfileEditor(row);
        });
        connect(m_profileList, &QListWidget::itemDoubleClicked, this, [controller, this](QListWidgetItem *item) {
            const int row = m_profileList->row(item);
            if (row < 0 || row >= m_profileEntries.size()) {
                return;
            }
            ProfileValues values;
            if (!parseProfileEntry(m_profileEntries.at(row), values)) {
                return;
            }
            const QString oldPattern = values.prefixedPattern;
            const bool titleKind = oldPattern.startsWith(QLatin1String("title:"));
            ProfileDialog dialog(this);
            dialog.setFields(titleKind, oldPattern.mid(titleKind ? 6 : 4), values.vibrancePercent);
            if (dialog.exec() != QDialog::Accepted) {
                return;
            }
            // The pattern is the identity: renaming means remove + add
            // (gains of the renamed rule are intentionally not carried).
            if (dialog.prefixedPattern().compare(oldPattern, Qt::CaseInsensitive) != 0) {
                controller->requestRemoveAppRule(oldPattern);
                controller->requestSetAppRuleFull(dialog.prefixedPattern(), dialog.ruleVibrance(),
                                                  values.redPercent / 100.0, values.greenPercent / 100.0,
                                                  values.bluePercent / 100.0);
            } else {
                controller->requestSetAppRule(dialog.prefixedPattern(), dialog.ruleVibrance());
            }
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
        auto *hotkeyLayout = new QVBoxLayout(hotkeyGroup);
        auto *hotkeyRow = new QHBoxLayout();
        m_shortcutEdit = new QKeySequenceEdit(hotkeyGroup);
        m_shortcutEdit->setKeySequence(controller->toggleShortcut());
        auto *clearButton = new QPushButton(tr("Clear"), hotkeyGroup);
        hotkeyRow->addWidget(m_shortcutEdit);
        hotkeyRow->addWidget(clearButton);
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
        connect(clearButton, &QPushButton::clicked, this, [controller, this, updateConflictLabel] {
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

        // Behaviour settings — one check per option, like Enabled.
        auto *behaviourGroup = new QGroupBox(tr("Behaviour"), central);
        auto *behaviourLayout = new QVBoxLayout(behaviourGroup);
        m_startHiddenCheck = new QCheckBox(tr("Open directly to the tray on startup"), behaviourGroup);
        m_startHiddenCheck->setChecked(storedStartHidden());
        m_autostartCheck = new QCheckBox(tr("Start with Plasma (autostart)"), behaviourGroup);
        m_autostartCheck->setChecked(autostartEnabled());
        m_primaryOnlyCheck = new QCheckBox(tr("Only affect the primary monitor"), behaviourGroup);
        behaviourLayout->addWidget(m_startHiddenCheck);
        behaviourLayout->addWidget(m_autostartCheck);
        behaviourLayout->addWidget(m_primaryOnlyCheck);
        outerLayout->addWidget(behaviourGroup);

        connect(m_startHiddenCheck, &QCheckBox::toggled, this, [](bool hidden) {
            saveStartHidden(hidden);
        });
        connect(m_autostartCheck, &QCheckBox::toggled, this, [this](bool on) {
            setAutostartEnabled(on);
            Q_EMIT autostartChanged(on);
        });
        connect(m_primaryOnlyCheck, &QCheckBox::toggled, this, [controller, this](bool on) {
            if (!m_updatingFromEffect) {
                controller->requestPrimaryOnly(on);
            }
        });
        connect(controller, &VibranceController::primaryOnlyFromEffect, this, [this](bool on) {
            m_updatingFromEffect = true;
            m_primaryOnlyCheck->setChecked(on);
            m_updatingFromEffect = false;
        });

        setCentralWidget(central);
        resize(780, 620);

        connect(m_slider, &QSlider::valueChanged, this, [controller, this](int percent) {
            updateValueLabel(percent);
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
            setPercent(int(value * 100.0));
            m_updatingFromEffect = false;
        });
        connect(controller, &VibranceController::enabledFromEffect, this, [this](bool enabled) {
            m_updatingFromEffect = true;
            m_enabledCheck->setChecked(enabled);
            m_updatingFromEffect = false;
        });
        connect(controller, &VibranceController::rgbGainsFromEffect, this, [this](double red, double green, double blue) {
            m_updatingRGB = true;
            setGains(int(red * 100.0), int(green * 100.0), int(blue * 100.0));
            m_updatingRGB = false;
        });
        connect(controller, &VibranceController::availableChanged, this, [this](bool available) {
            m_statusLabel->setText(available ? tr("Connected to the Way Vibrance effect.")
                                             : tr("The effect is not loaded in KWin."));
            m_slider->setEnabled(available);
            m_loadButton->setVisible(!available);
        });

        m_controller = controller;
        m_statusLabel->setText(controller->effectAvailable()
                                  ? tr("Connected to the Way Vibrance effect.")
                                  : tr("The effect is not loaded in KWin."));
        m_slider->setEnabled(controller->effectAvailable());
        m_loadButton->setVisible(!controller->effectAvailable());

        // Sync every control with the effect's real state now that all of
        // our connections exist (earlier state pushes may have gone unheard).
        controller->refreshState();
    }

private:
    void setPercent(int percent)
    {
        percent = qBound(kMinPercent, percent, kMaxPercent);
        m_slider->setValue(percent);
        updateValueLabel(percent);
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

    void rebuildProfileList(const QStringList &entries)
    {
        if (entries == m_profileEntries) {
            return; // echo of what we already show: keep the user's place
        }
        const int previousRow = m_profileList->currentRow();
        m_rebuildingProfiles = true;
        m_profileEntries = entries;
        m_profileList->clear();
        for (const QString &entry : entries) {
            ProfileValues values;
            if (!parseProfileEntry(entry, values)) {
                continue;
            }
            QString text = QStringLiteral("%1 — %2%")
                               .arg(friendlyAppName(values.prefixedPattern))
                               .arg(values.vibrancePercent);
            if (values.hasGains) {
                text += QStringLiteral("  [+RGB %1/%2/%3]")
                            .arg(values.redPercent)
                            .arg(values.greenPercent)
                            .arg(values.bluePercent);
            }
            auto *item = new QListWidgetItem(text, m_profileList);
            item->setToolTip(QStringLiteral("%1\n%2").arg(values.prefixedPattern, entry));
        }
        if (previousRow >= 0 && previousRow < m_profileList->count()) {
            m_profileList->setCurrentRow(previousRow);
        }
        m_rebuildingProfiles = false;
        loadProfileEditor(m_profileList->currentRow());
    }

    QString currentProfilePattern() const
    {
        const int row = m_profileList->currentRow();
        if (row < 0 || row >= m_profileEntries.size()) {
            return QString();
        }
        ProfileValues values;
        if (!parseProfileEntry(m_profileEntries.at(row), values)) {
            return QString();
        }
        return values.prefixedPattern;
    }

    void loadProfileEditor(int row)
    {
        if (m_rebuildingProfiles) {
            return;
        }
        ProfileValues values;
        if (row < 0 || row >= m_profileEntries.size() || !parseProfileEntry(m_profileEntries.at(row), values)) {
            m_editingPattern.clear();
            m_ruleEditor->setVisible(false);
            return;
        }
        const QString title = QStringLiteral("%1  (%2)")
                                  .arg(friendlyAppName(values.prefixedPattern), values.prefixedPattern);
        // Do not wrestle the sliders away from a user mid-drag on this rule:
        // the echo simply refreshes the title and the list text.
        if (values.prefixedPattern == m_editingPattern && m_profilePushTimer->isActive()) {
            m_ruleNameLabel->setText(title);
            return;
        }
        m_editingPattern = values.prefixedPattern;
        m_loadingProfile = true;
        m_ruleVibSlider->setValue(values.vibrancePercent);
        m_ruleVibValue->setText(QStringLiteral("%1%").arg(values.vibrancePercent));
        m_ruleRedSlider->setValue(values.redPercent);
        m_ruleRedValue->setText(QStringLiteral("%1%").arg(values.redPercent));
        m_ruleGreenSlider->setValue(values.greenPercent);
        m_ruleGreenValue->setText(QStringLiteral("%1%").arg(values.greenPercent));
        m_ruleBlueSlider->setValue(values.bluePercent);
        m_ruleBlueValue->setText(QStringLiteral("%1%").arg(values.bluePercent));
        m_loadingProfile = false;
        m_ruleNameLabel->setText(title);
        m_ruleEditor->setVisible(true);
    }

    void pushProfileEdits()
    {
        if (m_editingPattern.isEmpty() || !m_controller) {
            return;
        }
        m_controller->requestSetAppRuleFull(m_editingPattern,
                                            m_ruleVibSlider->value() / 100.0,
                                            m_ruleRedSlider->value() / 100.0,
                                            m_ruleGreenSlider->value() / 100.0,
                                            m_ruleBlueSlider->value() / 100.0);
    }

    VibranceController *m_controller = nullptr;
    QSlider *m_slider = nullptr;
    QSlider *m_redSlider = nullptr;
    QSlider *m_greenSlider = nullptr;
    QSlider *m_blueSlider = nullptr;
    QGroupBox *m_profileGroup = nullptr;
    QWidget *m_sidePane = nullptr;
    QListWidget *m_profileList = nullptr;
    QStringList m_profileEntries;
    QGroupBox *m_ruleEditor = nullptr;
    QLabel *m_ruleNameLabel = nullptr;
    QSlider *m_ruleVibSlider = nullptr;
    QLabel *m_ruleVibValue = nullptr;
    QSlider *m_ruleRedSlider = nullptr;
    QLabel *m_ruleRedValue = nullptr;
    QSlider *m_ruleGreenSlider = nullptr;
    QLabel *m_ruleGreenValue = nullptr;
    QSlider *m_ruleBlueSlider = nullptr;
    QLabel *m_ruleBlueValue = nullptr;
    QTimer *m_profilePushTimer = nullptr;
    QString m_editingPattern;
    bool m_loadingProfile = false;
    bool m_rebuildingProfiles = false;

public:
    void setAutostartChecked(bool on)
    {
        if (m_autostartCheck) {
            m_autostartCheck->setChecked(on);
        }
    }

Q_SIGNALS:
    void autostartChanged(bool enabled);

private:
    QLabel *m_valueLabel = nullptr;
    QLabel *m_redLabel = nullptr;
    QLabel *m_greenLabel = nullptr;
    QLabel *m_blueLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QCheckBox *m_enabledCheck = nullptr;
    QCheckBox *m_startHiddenCheck = nullptr;
    QCheckBox *m_autostartCheck = nullptr;
    QCheckBox *m_primaryOnlyCheck = nullptr;
    QPushButton *m_loadButton = nullptr;
    QKeySequenceEdit *m_shortcutEdit = nullptr;
    QLabel *m_shortcutInfo = nullptr;
    bool m_updatingFromEffect = false;
    bool m_updatingRGB = false;
    bool m_updatingShortcut = false;
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

    KAboutData about(QStringLiteral("org.kde.wayvibrance-tray"),
                     QStringLiteral("Way Vibrance"),
                     QStringLiteral("0.3.0"),
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
    QObject::connect(enabledAction, &QAction::toggled, &controller, &VibranceController::requestVibranceEnabled);
    QObject::connect(&controller, &VibranceController::enabledFromEffect, enabledAction, &QAction::setChecked);

    menu->addSeparator();
    for (int percent : {100, 125, 150, 200}) {
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
    auto *quitAction = menu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")), QObject::tr("Quit"));
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);

    QObject::connect(&controller, &VibranceController::vibranceFromEffect, icon, [icon](double value) {
        icon->setToolTip(QStringLiteral("Way Vibrance — %1%").arg(int(value * 100.0)));
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
