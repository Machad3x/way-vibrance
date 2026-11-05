/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibranceeffect.h"

#include <opengl/glshader.h>
#include <opengl/glshadermanager.h>

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
static constexpr auto s_shaderPath = ":/effects/wayvibrance/shaders/wayvibrance.frag";

VibranceEffect::VibranceEffect()
{
    m_persistTimer = new QTimer(this);
    m_persistTimer->setSingleShot(true);
    m_persistTimer->setInterval(500);
    connect(m_persistTimer, &QTimer::timeout, this, &VibranceEffect::persist);

    reloadConfig();

    connect(effects, &EffectsHandler::windowAdded, this, &VibranceEffect::onWindowAdded);

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
    // KWin's OpenGL scene renderer applies the saturation uniform natively
    // and GLShader-based offscreen passes are only guaranteed there.
    // Version tolerance: gate on the compositor's runtime capability, never
    // on a KWin version number. (TODO: accept Vulkan compositing once its
    // item renderer applies the saturation trait, to avoid a silent no-op.)
    return effects->isOpenGLCompositing();
}

bool VibranceEffect::isActive() const
{
    return m_enabled && (!qFuzzyCompare(m_vibrance, 1.0) || rgbGainsActive());
}

void VibranceEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)
    reloadConfig();
}

auto VibranceEffect::drawWindow(const RenderTarget &renderTarget,
                                const RenderViewport &viewport,
                                EffectWindow *w,
                                int mask,
                                const WvPaintRegion &region,
                                WindowPaintData &data)
    -> decltype(effects->drawWindow(renderTarget, viewport, w, mask, region, data))
{
    if (windowInScope(w)) {
        data.multiplySaturation(m_vibrance);
        // Windows not redirected (RGB gains neutral) are forwarded straight
        // to effects->drawWindow() by the base class — no grab, no extra pass.
        return OffscreenEffect::drawWindow(renderTarget, viewport, w, mask, region, data);
    }
    // "Only primary monitor" mode and this window is elsewhere: bypass both
    // the uniform and the offscreen pass, drawing it completely untouched.
    return effects->drawWindow(renderTarget, viewport, w, mask, region, data);
}

bool VibranceEffect::windowInScope(EffectWindow *w) const
{
    if (!m_primaryOnly) {
        return true;
    }
    // KWin exposes no public "primary output" to effects; the active screen
    // (the output holding the pointer) is the closest stable equivalent and
    // matches typical single-monitor gaming setups.
    return w->screen() && w->screen() == effects->activeScreen();
}

void VibranceEffect::reloadConfig()
{
    auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    config->reparseConfiguration();
    KConfigGroup cfg(config, QStringLiteral("Effect-%1").arg(QString::fromLatin1(s_configName)));
    const qreal vibrance = cfg.readEntry("Vibrance", 1.25);
    const bool enabled = cfg.readEntry("Enabled", true);
    const bool primaryOnly = cfg.readEntry("PrimaryOnly", false);
    const qreal red = cfg.readEntry("RedGain", 1.0);
    const qreal green = cfg.readEntry("GreenGain", 1.0);
    const qreal blue = cfg.readEntry("BlueGain", 1.0);

    m_vibrance = qBound<qreal>(0.0, vibrance, 4.0);
    m_enabled = enabled;
    m_primaryOnly = primaryOnly;
    m_rgbGains = QVector3D(static_cast<float>(qBound(0.0, red, 4.0)),
                           static_cast<float>(qBound(0.0, green, 4.0)),
                           static_cast<float>(qBound(0.0, blue, 4.0)));

    updateShaderUniforms();
    updateRedirects();

    Q_EMIT vibranceChanged(m_vibrance);
    Q_EMIT enabledChanged(m_enabled);
    Q_EMIT rgbGainsChanged(m_rgbGains.x(), m_rgbGains.y(), m_rgbGains.z());
    Q_EMIT primaryOnlyChanged(m_primaryOnly);

    if (effects) {
        effects->addRepaintFull();
    }
}

bool VibranceEffect::rgbGainsActive() const
{
    return !qFuzzyCompare(m_rgbGains, QVector3D(1.0f, 1.0f, 1.0f));
}

/**
 * Generates the RGB-gain shader on first need (the GL context is guaranteed
 * current once KWin drives the effect), only while gains are actually used.
 * If generation fails the effect degrades gracefully: vibrance keeps working
 * through the built-in uniform and the gains simply do nothing.
 */
void VibranceEffect::ensureShader()
{
    if (m_shader || m_shaderFailed) {
        return;
    }
    m_shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(),
                                                                 QLatin1String(s_shaderPath));
    if (!m_shader) {
        m_shaderFailed = true;
        qWarning() << "Way Vibrance: failed to load the RGB gain shader" << QLatin1String(s_shaderPath)
                   << "- per-channel gains will be ignored in this session.";
        return;
    }
    ShaderBinder binder{m_shader.get()};
    m_shader->setUniform("rgbGain", m_rgbGains);
}

void VibranceEffect::updateShaderUniforms()
{
    if (m_shader) {
        ShaderBinder binder{m_shader.get()};
        m_shader->setUniform("rgbGain", m_rgbGains);
    }
}

void VibranceEffect::updateRedirects()
{
    const bool shouldRedirect = m_enabled && rgbGainsActive();
    if (shouldRedirect) {
        ensureShader();
    }
    const bool actuallyRedirect = shouldRedirect && m_shader;
    if (actuallyRedirect == m_redirecting) {
        return;
    }
    m_redirecting = actuallyRedirect;

    const auto windows = effects->stackingOrder();
    if (shouldRedirect) {
        for (EffectWindow *w : windows) {
            redirect(w);
            setShader(w, m_shader.get());
        }
    } else {
        for (EffectWindow *w : windows) {
            unredirect(w);
        }
    }
    effects->addRepaintFull();
}

void VibranceEffect::onWindowAdded(EffectWindow *w)
{
    if (m_redirecting) {
        redirect(w);
        setShader(w, m_shader.get());
    }
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
    updateRedirects();
    Q_EMIT enabledChanged(m_enabled);
    m_persistTimer->start();
    effects->addRepaintFull();
}

bool VibranceEffect::vibranceEnabled() const
{
    return m_enabled;
}

void VibranceEffect::toggleVibranceEnabled()
{
    setVibranceEnabled(!m_enabled);
}

void VibranceEffect::setPrimaryOnly(bool primaryOnly)
{
    if (primaryOnly == m_primaryOnly) {
        return;
    }
    m_primaryOnly = primaryOnly;
    Q_EMIT primaryOnlyChanged(m_primaryOnly);
    m_persistTimer->start();
    effects->addRepaintFull();
}

bool VibranceEffect::primaryOnly() const
{
    return m_primaryOnly;
}

void VibranceEffect::setRGBGains(qreal red, qreal green, qreal blue)
{
    QVector3D gains(static_cast<float>(qBound(0.0, red, 4.0)),
                   static_cast<float>(qBound(0.0, green, 4.0)),
                   static_cast<float>(qBound(0.0, blue, 4.0)));
    if (qFuzzyCompare(gains, m_rgbGains)) {
        return;
    }
    m_rgbGains = gains;
    updateShaderUniforms();
    updateRedirects();
    Q_EMIT rgbGainsChanged(m_rgbGains.x(), m_rgbGains.y(), m_rgbGains.z());
    m_persistTimer->start();
    effects->addRepaintFull();
}

qreal VibranceEffect::redGain() const
{
    return m_rgbGains.x();
}

qreal VibranceEffect::greenGain() const
{
    return m_rgbGains.y();
}

qreal VibranceEffect::blueGain() const
{
    return m_rgbGains.z();
}

void VibranceEffect::persist()
{
    auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    KConfigGroup cfg(config, QStringLiteral("Effect-%1").arg(QString::fromLatin1(s_configName)));
    cfg.writeEntry("Vibrance", m_vibrance);
    cfg.writeEntry("Enabled", m_enabled);
    cfg.writeEntry("PrimaryOnly", m_primaryOnly);
    cfg.writeEntry("RedGain", static_cast<qreal>(m_rgbGains.x()));
    cfg.writeEntry("GreenGain", static_cast<qreal>(m_rgbGains.y()));
    cfg.writeEntry("BlueGain", static_cast<qreal>(m_rgbGains.z()));
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
    QObject::connect(m_effect, &VibranceEffect::rgbGainsChanged, this, &WayVibranceAdaptor::rgbGainsChanged);
    QObject::connect(m_effect, &VibranceEffect::primaryOnlyChanged, this, &WayVibranceAdaptor::primaryOnlyChanged);
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

void WayVibranceAdaptor::toggleEnabled()
{
    m_effect->toggleVibranceEnabled();
}

bool WayVibranceAdaptor::primaryOnly() const
{
    return m_effect->primaryOnly();
}

void WayVibranceAdaptor::setPrimaryOnly(bool primaryOnly)
{
    m_effect->setPrimaryOnly(primaryOnly);
}

qreal WayVibranceAdaptor::redGain() const
{
    return m_effect->redGain();
}

qreal WayVibranceAdaptor::greenGain() const
{
    return m_effect->greenGain();
}

qreal WayVibranceAdaptor::blueGain() const
{
    return m_effect->blueGain();
}

void WayVibranceAdaptor::setRGBGains(qreal red, qreal green, qreal blue)
{
    m_effect->setRGBGains(red, green, blue);
}

} // namespace KWin
