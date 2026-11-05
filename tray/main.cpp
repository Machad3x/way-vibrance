/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Way Vibrance tray controller: the Linux/Wayland (KDE) counterpart of
// vibrance-gui. Talks to the "wayvibrance" KWin effect over D-Bus for
// flicker-free live adjustments.

#include <QApplication>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

static constexpr auto s_effectId = "wayvibrance";
static constexpr auto s_vibranceService = "org.wayvibrance";
static constexpr auto s_vibrancePath = "/Vibrance";
static constexpr auto s_vibranceInterface = "org.wayvibrance";
static constexpr auto s_trayService = "org.wayvibrance.Tray";
static constexpr int kMinPercent = 0;
static constexpr int kMaxPercent = 250;
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

    void requestRGBGains(double red, double green, double blue)
    {
        m_pendingRed = red;
        m_pendingGreen = green;
        m_pendingBlue = blue;
        m_rgbPending = true;
        m_rgbTimer.start();
    }

Q_SIGNALS:
    void availableChanged(bool available);
    /** Emitted when the value changed remotely (effect/CLI), not by this UI. */
    void vibranceFromEffect(double value);
    void enabledFromEffect(bool enabled);
    void rgbGainsFromEffect(double red, double green, double blue);

private Q_SLOTS:
    void onVibranceChanged(double value)
    {
        Q_EMIT vibranceFromEffect(value);
    }

    void onEnabledChanged(bool enabled)
    {
        Q_EMIT enabledFromEffect(enabled);
    }

    void onRGBGainsChanged(double red, double green, double blue)
    {
        Q_EMIT rgbGainsFromEffect(red, green, blue);
    }

private:
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
        bus.connect(QString::fromLatin1(s_vibranceService),
                    QString::fromLatin1(s_vibrancePath),
                    QString::fromLatin1(s_vibranceInterface),
                    QStringLiteral("vibranceChanged(double)"),
                    this,
                    SLOT(onVibranceChanged(double)));
        bus.connect(QString::fromLatin1(s_vibranceService),
                    QString::fromLatin1(s_vibrancePath),
                    QString::fromLatin1(s_vibranceInterface),
                    QStringLiteral("enabledChanged(bool)"),
                    this,
                    SLOT(onEnabledChanged(bool)));
        bus.connect(QString::fromLatin1(s_vibranceService),
                    QString::fromLatin1(s_vibrancePath),
                    QString::fromLatin1(s_vibranceInterface),
                    QStringLiteral("rgbGainsChanged(double,double,double)"),
                    this,
                    SLOT(onRGBGainsChanged(double, double, double)));

        readInitialState();
        Q_EMIT availableChanged(true);
    }

    void detachInterface()
    {
        if (!m_interface) {
            return;
        }
        auto bus = QDBusConnection::sessionBus();
        bus.disconnect(QString::fromLatin1(s_vibranceService),
                       QString::fromLatin1(s_vibrancePath),
                       QString::fromLatin1(s_vibranceInterface),
                       QStringLiteral("vibranceChanged(double)"),
                       this,
                       SLOT(onVibranceChanged(double)));
        bus.disconnect(QString::fromLatin1(s_vibranceService),
                       QString::fromLatin1(s_vibrancePath),
                       QString::fromLatin1(s_vibranceInterface),
                       QStringLiteral("enabledChanged(bool)"),
                       this,
                       SLOT(onEnabledChanged(bool)));
        bus.disconnect(QString::fromLatin1(s_vibranceService),
                       QString::fromLatin1(s_vibrancePath),
                       QString::fromLatin1(s_vibranceInterface),
                       QStringLiteral("rgbGainsChanged(double,double,double)"),
                       this,
                       SLOT(onRGBGainsChanged(double, double, double)));
        delete m_interface;
        m_interface = nullptr;
        m_lastSent = -1.0;
        Q_EMIT availableChanged(false);
    }

    void readInitialState()
    {
        const QDBusReply<double> value = m_interface->call(QStringLiteral("vibrance"));
        if (value.isValid()) {
            onVibranceChanged(value.value());
        }
        const QDBusReply<bool> enabled = m_interface->call(QStringLiteral("enabled"));
        if (enabled.isValid()) {
            onEnabledChanged(enabled.value());
        }
        const QDBusReply<double> red = m_interface->call(QStringLiteral("redGain"));
        const QDBusReply<double> green = m_interface->call(QStringLiteral("greenGain"));
        const QDBusReply<double> blue = m_interface->call(QStringLiteral("blueGain"));
        if (red.isValid() && green.isValid() && blue.isValid()) {
            onRGBGainsChanged(red.value(), green.value(), blue.value());
        }
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
    QDBusServiceWatcher m_watcher;
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
        auto *outerLayout = new QVBoxLayout(central);

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

        m_loadButton = new QPushButton(tr("Load effect into KWin"), central);
        connect(m_loadButton, &QPushButton::clicked, this, [controller] { controller->ensureEffectLoaded(); });
        outerLayout->addWidget(m_loadButton);

        setCentralWidget(central);
        resize(420, 340);

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

    VibranceController *m_controller = nullptr;
    QSlider *m_slider = nullptr;
    QSlider *m_redSlider = nullptr;
    QSlider *m_greenSlider = nullptr;
    QSlider *m_blueSlider = nullptr;
    QLabel *m_valueLabel = nullptr;
    QLabel *m_redLabel = nullptr;
    QLabel *m_greenLabel = nullptr;
    QLabel *m_blueLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QCheckBox *m_enabledCheck = nullptr;
    QPushButton *m_loadButton = nullptr;
    bool m_updatingFromEffect = false;
    bool m_updatingRGB = false;
};

static QString autostartFilePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("autostart/wayvibrance.desktop"));
}

static bool autostartEnabled()
{
    return QFile::exists(autostartFilePath());
}

static void setAutostartEnabled(bool enabled)
{
    if (enabled) {
        QFile file(autostartFilePath());
        QDir().mkpath(QFileInfo(file).absolutePath());
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=Way Vibrance\n"
                       "Exec=wayvibrance-tray\n"
                       "X-GNOME-Autostart-enabled=true\n");
        }
    } else {
        QFile::remove(autostartFilePath());
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("wayvibrance-tray"));
    QApplication::setOrganizationName(QStringLiteral("wayvibrance"));
    QApplication::setApplicationDisplayName(QStringLiteral("Way Vibrance"));
    QApplication::setQuitOnLastWindowClosed(false);

    // Single instance: if a tray is already running, quit quietly.
    if (!QDBusConnection::sessionBus().registerService(QLatin1String(s_trayService))) {
        qInfo() << "Way Vibrance tray is already running.";
        return 0;
    }

    VibranceController controller;
    TrayWindow window(&controller);

    auto *icon = new QSystemTrayIcon(QIcon::fromTheme(QStringLiteral("preferences-desktop-color")), &app);
    auto *menu = new QMenu(&window);

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
    QObject::connect(autostartAction, &QAction::toggled, [](bool enabled) {
        setAutostartEnabled(enabled);
    });

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
    // When it comes up, the D-Bus handshake pushes the persisted value to the UI.
    controller.ensureEffectLoaded();

    return app.exec();
}

#include "main.moc"
