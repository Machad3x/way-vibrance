/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibranceeffect.h"

#include <window.h>

#include <opengl/glshader.h>
#include <opengl/glshadermanager.h>

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDebug>
#include <QDBusConnection>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>

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
static constexpr auto s_rulesGroupName = "WayVibranceAppRules";

VibranceEffect::VibranceEffect()
{
    m_persistTimer = new QTimer(this);
    m_persistTimer->setSingleShot(true);
    m_persistTimer->setInterval(500);
    connect(m_persistTimer, &QTimer::timeout, this, &VibranceEffect::persist);

    reloadConfig();

    connect(effects, &EffectsHandler::windowAdded, this, &VibranceEffect::onWindowAdded);
    connect(effects, &EffectsHandler::windowDeleted, this, &VibranceEffect::onWindowDeleted);
    connect(effects, &EffectsHandler::windowActivated, this, &VibranceEffect::onWindowActivated);

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
    return m_enabled
        && (!qFuzzyCompare(m_vibrance, 1.0) || rgbGainsActive() || !m_rules.isEmpty());
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
        data.multiplySaturation(vibranceForWindow(w));
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

    m_vibrance = qBound<qreal>(0.0, vibrance, 2.0);
    m_enabled = enabled;
    m_primaryOnly = primaryOnly;
    m_rgbGains = QVector3D(static_cast<float>(qBound(0.0, red, 2.0)),
                           static_cast<float>(qBound(0.0, green, 2.0)),
                           static_cast<float>(qBound(0.0, blue, 2.0)));

    m_rules.clear();
    const KConfigGroup rulesCfg = cfg.group(QString::fromLatin1(s_rulesGroupName));
    const QMap<QString, QString> entries = rulesCfg.entryMap();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        AppRule rule;
        if (!parseRule(QUrl::fromPercentEncoding(it.key().toLatin1()) + QLatin1Char('|') + it.value(),
                       rule)) {
            continue;
        }
        rule.vibrance = qBound<qreal>(0.0, rule.vibrance, 2.0);
        rule.gains = QVector3D(qBound(0.0f, rule.gains.x(), 2.0f),
                               qBound(0.0f, rule.gains.y(), 2.0f),
                               qBound(0.0f, rule.gains.z(), 2.0f));
        m_rules.append(rule);
    }
    rebuildMatchers();
    invalidateMatchCache();

    updateShaderUniforms();
    updateRedirects();

    Q_EMIT vibranceChanged(m_vibrance);
    Q_EMIT enabledChanged(m_enabled);
    Q_EMIT rgbGainsChanged(m_rgbGains.x(), m_rgbGains.y(), m_rgbGains.z());
    Q_EMIT primaryOnlyChanged(m_primaryOnly);
    Q_EMIT appRulesChanged(appRules());

    if (effects) {
        effects->addRepaintFull();
    }
}

bool VibranceEffect::rgbGainsActive() const
{
    return !qFuzzyCompare(m_rgbGains, QVector3D(1.0f, 1.0f, 1.0f));
}

// ---------------------------------------------------------------------------
// Per-app profiles
// ---------------------------------------------------------------------------

/**
 * Converts a user glob into an anchored, case-insensitive regex. Only '*'
 * and '?' act as wildcards; everything else is literal.
 */
static QRegularExpression globToMatcher(const QString &glob)
{
    QString regex;
    regex.reserve(glob.size() + 8);
    int literalStart = 0;
    for (int i = 0; i < glob.size(); ++i) {
        const QChar c = glob.at(i);
        if (c != QLatin1Char('*') && c != QLatin1Char('?')) {
            continue;
        }
        if (i > literalStart) {
            regex += QRegularExpression::escape(glob.mid(literalStart, i - literalStart));
        }
        regex += (c == QLatin1Char('*')) ? QStringLiteral(".*") : QStringLiteral(".");
        literalStart = i + 1;
    }
    if (literalStart < glob.size()) {
        regex += QRegularExpression::escape(glob.mid(literalStart));
    }
    return QRegularExpression(QRegularExpression::anchoredPattern(regex),
                              QRegularExpression::CaseInsensitiveOption);
}

QString VibranceEffect::serializedRule(const AppRule &rule)
{
    QString out = QStringLiteral("%1%2|%3")
                      .arg(rule.titleMatch ? QStringLiteral("title:") : QStringLiteral("app:"),
                           rule.pattern)
                      .arg(rule.vibrance, 0, 'g', 6);
    // Field layout: kind:pattern|vibrance[|r,g,b[|name]] — the gains field is
    // emitted whenever a display name exists too, so the name always lands
    // in the fourth slot.
    if (rule.hasGains() || !rule.displayName.isEmpty()) {
        out += QStringLiteral("|%1,%2,%3")
                   .arg(rule.gains.x(), 0, 'g', 4)
                   .arg(rule.gains.y(), 0, 'g', 4)
                   .arg(rule.gains.z(), 0, 'g', 4);
    }
    if (!rule.displayName.isEmpty()) {
        out += QLatin1Char('|') + rule.displayName;
    }
    return out;
}

bool VibranceEffect::parseRule(const QString &serialized, AppRule &out)
{
    const QStringList fields = serialized.split(QLatin1Char('|'));
    if (fields.size() < 2 || fields.size() > 4) {
        return false;
    }
    bool ok = false;
    const qreal vibrance = fields.at(1).toDouble(&ok);
    if (!ok) {
        return false;
    }
    out.vibrance = vibrance;
    if (fields.size() >= 3) {
        const QStringList parts = fields.at(2).split(QLatin1Char(','));
        if (parts.size() != 3) {
            return false;
        }
        const qreal red = parts.at(0).toDouble(&ok);
        if (!ok) {
            return false;
        }
        const qreal green = parts.at(1).toDouble(&ok);
        if (!ok) {
            return false;
        }
        const qreal blue = parts.at(2).toDouble(&ok);
        if (!ok) {
            return false;
        }
        out.gains = QVector3D(static_cast<float>(red), static_cast<float>(green), static_cast<float>(blue));
    }
    if (fields.size() == 4) {
        out.displayName = fields.at(3);
    }
    QString head = fields.at(0);
    if (head.startsWith(QLatin1String("title:"))) {
        out.titleMatch = true;
        head = head.mid(6);
    } else if (head.startsWith(QLatin1String("app:"))) {
        out.titleMatch = false;
        head = head.mid(4);
    }
    if (head.trimmed().isEmpty()) {
        return false;
    }
    out.pattern = head;
    return true;
}

static QVector3D clampGains(qreal red, qreal green, qreal blue)
{
    return QVector3D(static_cast<float>(qBound(0.0, red, 2.0)),
                     static_cast<float>(qBound(0.0, green, 2.0)),
                     static_cast<float>(qBound(0.0, blue, 2.0)));
}

void VibranceEffect::rebuildMatchers()
{
    for (AppRule &rule : m_rules) {
        rule.matcher = globToMatcher(rule.pattern);
    }
}

void VibranceEffect::invalidateMatchCache()
{
    ++m_matchStamp;
    m_windowMatch.clear();
}

void VibranceEffect::applyRule(const QString &prefixedPattern, qreal vibrance,
                               const std::optional<QVector3D> &gains,
                               const std::optional<QString> &name)
{
    bool titleMatch = false;
    QString pattern = prefixedPattern.trimmed();
    if (pattern.startsWith(QLatin1String("title:"))) {
        titleMatch = true;
        pattern = pattern.mid(6).trimmed();
    } else if (pattern.startsWith(QLatin1String("app:"))) {
        pattern = pattern.mid(4).trimmed();
    }
    if (pattern.isEmpty() || pattern.contains(QLatin1Char('|'))) {
        qWarning() << "Way Vibrance: ignoring invalid app profile" << prefixedPattern;
        return;
    }
    vibrance = qBound<qreal>(0.0, vibrance, 2.0);
    QString cleanName;
    if (name.has_value()) {
        cleanName = name->trimmed();
        cleanName.remove(QLatin1Char('|'));
    }

    bool matched = false;
    for (AppRule &rule : m_rules) {
        if (rule.titleMatch == titleMatch && rule.pattern.compare(pattern, Qt::CaseInsensitive) == 0) {
            matched = true;
            const bool unchanged = qFuzzyCompare(rule.vibrance, vibrance)
                && (!gains.has_value() || qFuzzyCompare(rule.gains, *gains))
                && (!name.has_value() || rule.displayName == cleanName);
            if (unchanged) {
                return;
            }
            rule.vibrance = vibrance;
            if (gains.has_value()) {
                rule.gains = *gains;
            }
            if (name.has_value()) {
                rule.displayName = cleanName;
            }
            break;
        }
    }
    if (!matched) {
        AppRule rule;
        rule.pattern = pattern;
        rule.titleMatch = titleMatch;
        rule.vibrance = vibrance;
        if (gains.has_value()) {
            rule.gains = *gains;
        }
        if (name.has_value()) {
            rule.displayName = cleanName;
        }
        m_rules.append(rule);
    }

    rebuildMatchers();
    invalidateMatchCache();
    updateRedirects();
    pruneShaderPool();
    Q_EMIT appRulesChanged(appRules());
    m_persistTimer->start();
    effects->addRepaintFull();
}

void VibranceEffect::setAppRule(const QString &prefixedPattern, qreal vibrance)
{
    // Keeps whatever channel gains and display name the profile already carries.
    applyRule(prefixedPattern, vibrance, std::nullopt, std::nullopt);
}

void VibranceEffect::setAppRuleFull(const QString &prefixedPattern, qreal vibrance,
                                    qreal red, qreal green, qreal blue)
{
    applyRule(prefixedPattern, vibrance, clampGains(red, green, blue), std::nullopt);
}

void VibranceEffect::setAppRuleName(const QString &prefixedPattern, const QString &name)
{
    // Cosmetic-only edit: finds the existing rule and touches nothing else.
    bool titleMatch = false;
    QString pattern = prefixedPattern.trimmed();
    if (pattern.startsWith(QLatin1String("title:"))) {
        titleMatch = true;
        pattern = pattern.mid(6).trimmed();
    } else if (pattern.startsWith(QLatin1String("app:"))) {
        pattern = pattern.mid(4).trimmed();
    }
    QString cleanName = name.trimmed();
    cleanName.remove(QLatin1Char('|'));
    for (AppRule &rule : m_rules) {
        if (rule.titleMatch == titleMatch && rule.pattern.compare(pattern, Qt::CaseInsensitive) == 0) {
            if (rule.displayName == cleanName) {
                return;
            }
            rule.displayName = cleanName;
            Q_EMIT appRulesChanged(appRules());
            m_persistTimer->start();
            return;
        }
    }
    qWarning() << "Way Vibrance: cannot rename unknown app profile" << prefixedPattern;
}

void VibranceEffect::removeAppRule(const QString &prefixedPattern)
{
    bool titleMatch = false;
    QString pattern = prefixedPattern.trimmed();
    if (pattern.startsWith(QLatin1String("title:"))) {
        titleMatch = true;
        pattern = pattern.mid(6).trimmed();
    } else if (pattern.startsWith(QLatin1String("app:"))) {
        pattern = pattern.mid(4).trimmed();
    }
    const int removed = m_rules.removeIf([&](const AppRule &rule) {
        return rule.titleMatch == titleMatch && rule.pattern.compare(pattern, Qt::CaseInsensitive) == 0;
    });
    if (removed == 0) {
        return;
    }
    invalidateMatchCache();
    updateRedirects();
    pruneShaderPool();
    Q_EMIT appRulesChanged(appRules());
    m_persistTimer->start();
    effects->addRepaintFull();
}

void VibranceEffect::clearAppRules()
{
    if (m_rules.isEmpty()) {
        return;
    }
    m_rules.clear();
    invalidateMatchCache();
    updateRedirects();
    pruneShaderPool();
    Q_EMIT appRulesChanged(appRules());
    m_persistTimer->start();
    effects->addRepaintFull();
}

QStringList VibranceEffect::appRules() const
{
    QStringList out;
    out.reserve(m_rules.size());
    for (const AppRule &rule : m_rules) {
        out.append(serializedRule(rule));
    }
    return out;
}

QString VibranceEffect::lastFocusedAppId() const
{
    return m_lastFocusedAppId;
}

qreal VibranceEffect::vibranceForWindow(EffectWindow *w)
{
    if (m_rules.isEmpty()) {
        return m_vibrance;
    }
    WindowMatch &entry = m_windowMatch[w];
    const QString idClass = w->window() ? w->window()->resourceClass() : QString();
    const QString idName = w->window() ? w->window()->resourceName() : QString();
    const QString caption = w->caption();
    if (entry.stamp == m_matchStamp && entry.idA == idClass && entry.idB == idName && entry.caption == caption) {
        return entry.ruleIndex >= 0 ? m_rules[entry.ruleIndex].vibrance : m_vibrance;
    }
    entry.stamp = m_matchStamp;
    entry.idA = idClass;
    entry.idB = idName;
    entry.caption = caption;
    entry.ruleIndex = -1;
    for (int i = 0; i < m_rules.size(); ++i) {
        const AppRule &rule = m_rules.at(i);
        if (!rule.matcher.isValid()) {
            continue;
        }
        if (rule.titleMatch) {
            if (rule.matcher.match(caption).hasMatch()) {
                entry.ruleIndex = i;
                break;
            }
        } else if ((rule.matcher.match(idClass).hasMatch() && !idClass.isEmpty())
                   || (rule.matcher.match(idName).hasMatch() && !idName.isEmpty())) {
            entry.ruleIndex = i;
            break;
        }
    }
    return entry.ruleIndex >= 0 ? m_rules[entry.ruleIndex].vibrance : m_vibrance;
}

QVector3D VibranceEffect::gainsForWindow(EffectWindow *w)
{
    // Reuse the match entry filled by vibranceForWindow (drawWindow calls
    // them back to back, so this is a cache hit in practice).
    vibranceForWindow(w);
    if (m_rules.isEmpty()) {
        return m_rgbGains;
    }
    const auto it = m_windowMatch.constFind(w);
    if (it == m_windowMatch.constEnd() || it->ruleIndex < 0) {
        return m_rgbGains;
    }
    const AppRule &rule = m_rules.at(it->ruleIndex);
    return rule.hasGains() ? rule.gains : m_rgbGains;
}

void VibranceEffect::onWindowDeleted(EffectWindow *w)
{
    m_windowMatch.remove(w);
}

void VibranceEffect::onWindowActivated(EffectWindow *w)
{
    if (!w || !w->isNormalWindow() || !w->window()) {
        return;
    }
    QString id = w->window()->resourceClass();
    if (id.isEmpty()) {
        id = w->window()->resourceName();
    }
    // Our own windows must not shadow the game/app the user just alt+tabbed
    // through on the way to the tray controls.
    if (id.isEmpty() || id.contains(QLatin1String("wayvibrance"), Qt::CaseInsensitive)) {
        return;
    }
    m_lastFocusedAppId = id;
}

/**
 * Generates the RGB-gain shader on first need (the GL context is guaranteed
 * current once KWin drives the effect), only while gains are actually used.
 * If generation fails the effect degrades gracefully: vibrance keeps working
 * through the built-in uniform and the gains simply do nothing.
 */
bool VibranceEffect::ensureShader()
{
    if (m_shader) {
        return true;
    }
    if (m_shaderFailed) {
        return false;
    }
    m_shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(),
                                                                 QLatin1String(s_shaderPath));
    if (!m_shader) {
        m_shaderFailed = true;
        qWarning() << "Way Vibrance: failed to load the RGB gain shader" << QLatin1String(s_shaderPath)
                   << "- per-channel gains will be ignored in this session.";
        return false;
    }
    ShaderBinder binder{m_shader.get()};
    m_shader->setUniform("rgbGain", m_rgbGains);
    return true;
}

void VibranceEffect::updateShaderUniforms()
{
    if (m_shader) {
        ShaderBinder binder{m_shader.get()};
        m_shader->setUniform("rgbGain", m_rgbGains);
    }
}

QString VibranceEffect::gainsKey(const QVector3D &gains)
{
    return QStringLiteral("%1_%2_%3")
        .arg(qRound(gains.x() * 1000.0))
        .arg(qRound(gains.y() * 1000.0))
        .arg(qRound(gains.z() * 1000.0));
}

/**
 * One GLShader instance per distinct gain set: the offscreen pass takes its
 * values from the shader's uniforms, so a profile with its own gains needs
 * its own compiled instance (they share the same fragment source, and are
 * only created while some window actually uses them).
 */
GLShader *VibranceEffect::shaderForGains(const QVector3D &gains)
{
    if (!ensureShader()) {
        return nullptr;
    }
    if (qFuzzyCompare(gains, m_rgbGains)) {
        return m_shader.get();
    }
    const QString key = gainsKey(gains);
    const auto it = m_shaderPool.constFind(key);
    if (it != m_shaderPool.constEnd()) {
        return it.value().get();
    }
    auto fresh = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(),
                                                                   QLatin1String(s_shaderPath));
    if (!fresh) {
        return nullptr;
    }
    {
        ShaderBinder binder{fresh.get()};
        fresh->setUniform("rgbGain", gains);
    }
    GLShader *raw = fresh.get();
    m_shaderPool.insert(key, std::shared_ptr<GLShader>(std::move(fresh)));
    return raw;
}

void VibranceEffect::pruneShaderPool()
{
    // Free instances no effective window-gain set uses any more. Callers
    // refresh the per-window shaders first, so no window can still point
    // at a shader we drop here.
    QSet<QString> inUse;
    if (m_enabled && rgbGainsActive()) {
        inUse.insert(gainsKey(m_rgbGains)); // handled by m_shader, but the key is reserved
    }
    for (const AppRule &rule : m_rules) {
        if (m_enabled && rule.hasGains()) {
            inUse.insert(gainsKey(rule.gains));
        }
    }
    for (auto it = m_shaderPool.begin(); it != m_shaderPool.end();) {
        if (!inUse.contains(it.key())) {
            it = m_shaderPool.erase(it);
        } else {
            ++it;
        }
    }
}

void VibranceEffect::applyWindowRedirect(EffectWindow *w)
{
    const QVector3D gains = gainsForWindow(w);
    if (!m_enabled || qFuzzyCompare(gains, QVector3D(1.0f, 1.0f, 1.0f))) {
        unredirect(w);
        return;
    }
    if (GLShader *shader = shaderForGains(gains)) {
        redirect(w);
        setShader(w, shader);
    } else {
        unredirect(w);
    }
}

void VibranceEffect::updateRedirects()
{
    // Each window follows its own effective gain set (global or the first
    // matching profile's).
    bool anyRedirect = false;
    const auto windows = effects->stackingOrder();
    for (EffectWindow *w : windows) {
        applyWindowRedirect(w);
        const QVector3D gains = gainsForWindow(w);
        if (m_enabled && !qFuzzyCompare(gains, QVector3D(1.0f, 1.0f, 1.0f))) {
            anyRedirect = true;
        }
    }
    m_redirecting = anyRedirect;
    effects->addRepaintFull();
}

void VibranceEffect::onWindowAdded(EffectWindow *w)
{
    applyWindowRedirect(w);
}

void VibranceEffect::setVibrance(qreal value)
{
    value = qBound<qreal>(0.0, value, 2.0);
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
    QVector3D gains(static_cast<float>(qBound(0.0, red, 2.0)),
                   static_cast<float>(qBound(0.0, green, 2.0)),
                   static_cast<float>(qBound(0.0, blue, 2.0)));
    if (qFuzzyCompare(gains, m_rgbGains)) {
        return;
    }
    m_rgbGains = gains;
    updateShaderUniforms();
    updateRedirects();
    pruneShaderPool();
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

    // Per-app profiles live in a child group; keys are the percent-encoded
    // "kind:pattern", values "vibrance" plus "|r,g,b" when the profile
    // carries its own channel gains.
    cfg.group(QString::fromLatin1(s_rulesGroupName)).deleteGroup();
    if (!m_rules.isEmpty()) {
        KConfigGroup rulesCfg(config, QStringLiteral("Effect-%1").arg(QString::fromLatin1(s_configName)));
        rulesCfg = rulesCfg.group(QString::fromLatin1(s_rulesGroupName));
        for (const AppRule &rule : m_rules) {
            const QString id = serializedRule(rule);
            const int firstSep = id.indexOf(QLatin1Char('|'));
            rulesCfg.writeEntry(QString::fromLatin1(QUrl::toPercentEncoding(id.left(firstSep))),
                                id.mid(firstSep + 1));
        }
    }
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
    QObject::connect(m_effect, &VibranceEffect::appRulesChanged, this, &WayVibranceAdaptor::appRulesChanged);
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

QStringList WayVibranceAdaptor::appRules() const
{
    return m_effect->appRules();
}

void WayVibranceAdaptor::setAppRule(const QString &prefixedPattern, qreal vibrance)
{
    m_effect->setAppRule(prefixedPattern, vibrance);
}

void WayVibranceAdaptor::setAppRuleFull(const QString &prefixedPattern, qreal vibrance,
                                        qreal red, qreal green, qreal blue)
{
    m_effect->setAppRuleFull(prefixedPattern, vibrance, red, green, blue);
}

void WayVibranceAdaptor::setAppRuleName(const QString &prefixedPattern, const QString &name)
{
    m_effect->setAppRuleName(prefixedPattern, name);
}

void WayVibranceAdaptor::removeAppRule(const QString &prefixedPattern)
{
    m_effect->removeAppRule(prefixedPattern);
}

void WayVibranceAdaptor::clearAppRules()
{
    m_effect->clearAppRules();
}

QString WayVibranceAdaptor::lastFocusedAppId() const
{
    return m_effect->lastFocusedAppId();
}

} // namespace KWin
