/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibranceeffect.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDebug>
#include <QDBusConnection>
#include <QTimer>

// The drawWindow() hook this effect overrides is present with compatible
// semantics since KWin 6.0 (effects API 0.236). Fail with a clear message on
// older headers rather than a cryptic error; nothing here is pinned to an
// *upper* version, so later 6.x releases keep compiling unchanged.
#if !defined(KWIN_EFFECT_API_VERSION_MINOR) || KWIN_EFFECT_API_VERSION_MINOR < 236
#error "way-vibrance requires KWin 6.0 or newer (Window effects API with RenderTarget/RenderViewport)"
#endif

namespace KWin
{

static constexpr auto s_configName = "wayvibrance";
static constexpr auto s_dbusServiceName = "org.wayvibrance";
static constexpr auto s_dbusObjectPath = "/Vibrance";

VibranceEffect::VibranceEffect()
{
    reloadConfig();

    m_persistTimer = new QTimer(this);
    m_persistTimer->setSingleShot(true);
    m_persistTimer->setInterval(500);
    connect(m_persistTimer, &QTimer::timeout, this, &VibranceEffect::persist);

    QDBusConnection bus = QDBusConnection::sessionBus();
    m_dbusRegistered = bus.registerService(QString::fromLatin1(s_dbusServiceName));
    if (m_dbusRegistered) {
        // ExportAdaptors only exports QDBusAbstractAdaptor instances already
        // attached to the object, so the adaptor must be created *before*
        // registerObject(). It is parented here: when KWin unloads the effect
        // the adaptor dies with it and the next instance can re-register.
        new WayVibranceAdaptor(this);
        m_dbusRegistered = bus.registerObject(QString::fromLatin1(s_dbusObjectPath),
                                              this,
                                              QDBusConnection::ExportAdaptors);
        if (!m_dbusRegistered) {
            bus.unregisterService(QString::fromLatin1(s_dbusServiceName));
        }
    }
    if (!m_dbusRegistered) {
        qWarning() << "Way Vibrance: could not register the org.wayvibrance D-Bus interface."
                   << "Configure it through kwinrc [Effect-wayvibrance] instead.";
    }
}

VibranceEffect::~VibranceEffect()
{
    if (m_dbusRegistered) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QString::fromLatin1(s_dbusObjectPath));
        bus.unregisterService(QString::fromLatin1(s_dbusServiceName));
    }
}

bool VibranceEffect::supported()
{
    // KWin's OpenGL scene renderer applies the saturation uniform natively.
    // Version tolerance: gate on the compositor's runtime capability, never
    // on a KWin version number. (TODO: also accept Vulkan compositing once
    // its item renderer applies the saturation trait, to avoid a silent
    // no-op there.)
    return effects->isOpenGLCompositing();
}

bool VibranceEffect::isActive() const
{
    return !qFuzzyCompare(m_vibrance, 1.0) && m_enabled;
}

void VibranceEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)
    reloadConfig();
}

void VibranceEffect::reloadConfig()
{
    auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    config->reparseConfiguration();
    KConfigGroup cfg(config, QStringLiteral("Effect-%1").arg(QString::fromLatin1(s_configName)));
    const qreal vibrance = cfg.readEntry("Vibrance", 1.25);
    const bool enabled = cfg.readEntry("Enabled", true);

    m_vibrance = qBound<qreal>(0.0, vibrance, 4.0);
    m_enabled = enabled;

    Q_EMIT vibranceChanged(m_vibrance);
    Q_EMIT enabledChanged(m_enabled);

    if (effects) {
        effects->addRepaintFull();
    }
}

// Version tolerance: the trailing return type *derives* from the base call —
// void on KWin 6.0–6.7, bool on 6.8+ — and `return <void expr>` is valid in a
// cv-void function, so this single definition compiles on every 6.x release.
auto VibranceEffect::drawWindow(const RenderTarget &renderTarget,
                                const RenderViewport &viewport,
                                EffectWindow *w,
                                int mask,
                                const WvPaintRegion &region,
                                WindowPaintData &data)
    -> decltype(effects->drawWindow(renderTarget, viewport, w, mask, region, data))
{
    data.multiplySaturation(m_vibrance);
    return effects->drawWindow(renderTarget, viewport, w, mask, region, data);
}

void VibranceEffect::setVibrance(qreal value)
{
    value = qBound<qreal>(0.0, value, 4.0);
    if (qFuzzyCompare(value, m_vibrance)) {
        return;
    }
    m_vibrance = value;
    Q_EMIT vibranceChanged(m_vibrance);
    m_persistTimer->start();
    effects->addRepaintFull();
}

qreal VibranceEffect::vibrance() const
{
    return m_vibrance;
}

void VibranceEffect::setVibranceEnabled(bool enabled)
{
    if (enabled == m_enabled) {
        return;
    }
    m_enabled = enabled;
    Q_EMIT enabledChanged(m_enabled);
    m_persistTimer->start();
    effects->addRepaintFull();
}

bool VibranceEffect::vibranceEnabled() const
{
    return m_enabled;
}

void VibranceEffect::persist()
{
    auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    KConfigGroup cfg(config, QStringLiteral("Effect-%1").arg(QString::fromLatin1(s_configName)));
    cfg.writeEntry("Vibrance", m_vibrance);
    cfg.writeEntry("Enabled", m_enabled);
    config->sync();
}

WayVibranceAdaptor::WayVibranceAdaptor(VibranceEffect *effect)
    : QDBusAbstractAdaptor(effect)
    , m_effect(effect)
{
    // Forward the effect's change signals over D-Bus so controllers (tray,
    // CLI, future profile daemon) stay in sync no matter who changed it.
    QObject::connect(m_effect, &VibranceEffect::vibranceChanged, this, &WayVibranceAdaptor::vibranceChanged);
    QObject::connect(m_effect, &VibranceEffect::enabledChanged, this, &WayVibranceAdaptor::enabledChanged);
}

qreal WayVibranceAdaptor::vibrance() const
{
    return m_effect->vibrance();
}

void WayVibranceAdaptor::setVibrance(qreal value)
{
    m_effect->setVibrance(value);
}

bool WayVibranceAdaptor::enabled() const
{
    return m_effect->vibranceEnabled();
}

void WayVibranceAdaptor::setEnabled(bool enabled)
{
    m_effect->setVibranceEnabled(enabled);
}

} // namespace KWin
