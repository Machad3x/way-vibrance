/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "globmatch_p.h"
#include "vibranceeffect.h"
#include "common_limits.h"

#include <window.h>

// Version-tolerant includes for the output classes the screen API talks
// about: KWin renamed/restructured them across 6.x (Screen → Output →
// LogicalOutput). We only ever touch them through deduced `auto`, so
// whatever the installed headers define is enough.
#if __has_include(<core/output.h>)
#include <core/output.h>
#endif
#if __has_include(<core/screen.h>)
#include <core/screen.h>
#endif

#include <opengl/glshader.h>
#include <opengl/glshadermanager.h>

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDebug>
#include <QDBusConnection>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>

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
static constexpr auto s_rulesOrderKey = "WayVibranceAppRuleOrder";

// Shader generation is retried at most this many times, with a small delay
// between attempts so a transient context/compile failure is not hammered
// once per frame. After the last failure the gains path stays disabled until
// KWin reloads the effect.
static constexpr int kMaxShaderAttempts = 3;
static constexpr qint64 kShaderRetryDelayMs = 1000;

// Local-DoS guards (a session client must not be able to grow unbounded
// state or compile cost inside the compositor) now live in common_limits.h
// (at the repo root, shared with tray/ and tests/).
// Compiled GLShader variants cached for profile gains. Comfortably above
// any legitimate working set (kMaxAppRules + the global set), yet bounded
// so endless gain-editing churn cannot grow the pool (or the GPU memory
// and shader-compiler work behind it) without limit.
// (kMaxShaderVariants also comes from common_limits.h) // > kMaxAppRules + 1 by design:
// a variant only needs to exist while some window applies its gains, so this
// ceiling exceeds the largest legitimate working set — the substitution and
// refuse branches below are defense-in-depth, unreachable via the API.
// A saturated pool may substitute the nearest cached variant only if no
// channel moves by more than this (2% of the 0-2 gain range). Beyond it
// we refuse the substitution outright and drop the offscreen pass for
// that window (profile saturation still applies) — silently shifting a
// user's colors by more than 2% would be a feature regression wearing
// security clothing.
// (kShaderNearestMaxDelta also comes from common_limits.h)

// qBound happily forwards NaN (every comparison against it is false), so
// finiteness is validated before clamping everywhere a factor enters.
static QVector3D clampGains(qreal red, qreal green, qreal blue);

static bool validFactor(qreal v, qreal &clampedOut)
{
    if (!std::isfinite(v)) {
        return false;
    }
    clampedOut = qBound<qreal>(0.0, v, 2.0);
    return true;
}

VibranceEffect::VibranceEffect()
{
    m_persistTimer = new QTimer(this);
    m_persistTimer->setSingleShot(true);
    m_persistTimer->setInterval(500);
    connect(m_persistTimer, &QTimer::timeout, this, &VibranceEffect::persist);

    reloadConfig();

    connect(effects, &EffectsHandler::windowDeleted, this, &VibranceEffect::onWindowDeleted);
    connect(effects, &EffectsHandler::windowActivated, this, &VibranceEffect::onWindowActivated);

    QDBusConnection bus = QDBusConnection::sessionBus();
    m_dbusRegistered = bus.registerService(QString::fromLatin1(s_dbusServiceName));
    if (m_dbusRegistered) {
        // ExportAdaptors only exports QDBusAbstractAdaptor instances already
        // attached to the object, so the adaptor must be created *before*
        // registerObject(). It is parented here: when KWin unloads the effect
        // the adaptor dies with it and the next instance can re-register.
        //
        // Trust boundary: this is a per-user session-bus interface with no
        // token. Any process under the same UID can call it, which is why
        // every setter bounds/validates its input and none of them grants
        // anything beyond this user's colour settings. The tray protects
        // itself separately by trusting only the compositor-owned name.
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
    resolveWindow(w);
    WindowMatch &entry = m_windowMatch[w]; // filled by resolveWindow()
    const bool inScope = windowInScope(w);

    // Deferred global-uniform upload: this is the one place where the GL
    // context is guaranteed current (values changed via D-Bus/config are
    // only marked dirty there — compiling shaders or setting uniforms
    // from a D-Bus callback races the compositor's context and silently
    // no-ops, which used to make profiles "stop responding").
    if (m_uniformsDirty && m_shader) {
        ShaderBinder binder{m_shader.get()};
        m_shader->setUniform("rgbGain", m_rgbGains);
        m_uniformsDirty = false;
    }

    qreal vib = m_vibrance;
    QVector3D gains = m_rgbGains;
    if (inScope && entry.ruleIndex >= 0) {
        const AppRule &rule = m_rules.at(entry.ruleIndex);
        vib = rule.vibrance;
        if (rule.hasGains()) {
            gains = rule.gains;
        }
    }

    if (inScope) {
        data.multiplySaturation(vib);
        GLShader *desired = nullptr;
        if (!qFuzzyCompare(gains, QVector3D(1.0f, 1.0f, 1.0f))) {
            desired = shaderForGains(gains); // generates on demand, in-context
        }
        if (desired != entry.appliedShader) {
            if (desired) {
                redirect(w);
                setShader(w, desired);
                entry.appliedShader = desired;
            } else {
                unredirect(w);
                entry.appliedShader = nullptr;
            }
        }
        // Windows not redirected (RGB gains neutral) are forwarded straight
        // to effects->drawWindow() by the base class — no grab, no extra pass.
        return OffscreenEffect::drawWindow(renderTarget, viewport, w, mask, region, data);
    }

    // "Only primary monitor" mode and this window is elsewhere: bypass both
    // the uniform and the offscreen pass, drawing it completely untouched.
    if (entry.appliedShader) {
        unredirect(w);
        entry.appliedShader = nullptr;
    }
    return effects->drawWindow(renderTarget, viewport, w, mask, region, data);
}

bool VibranceEffect::windowInScope(EffectWindow *w) const
{
    if (!m_primaryOnly) {
        return true;
    }
    // Fixed, pointer-independent target: the output picked by name (tray
    // dropdown / CLI), falling back to the first output. KWin exposes no
    // compositor-wide "primary" to effects, and activeScreen() follows the
    // mouse — which is not what "primary monitor" means. Version-tolerant:
    // every type here is deduced (auto), so whatever the installed KWin
    // calls its output classes, the effect compiles unchanged.
    const auto screens = effects->screens();
    if (screens.isEmpty()) {
        return true;
    }
    const auto windowScreen = w->screen();
    if (!windowScreen) {
        return false;
    }
    if (!m_primaryOutputName.isEmpty()) {
        for (const auto screen : screens) {
            if (screen->name() == m_primaryOutputName) {
                return windowScreen == screen;
            }
        }
    }
    return windowScreen == screens.first(); // unset or unplugged selection
}

QString VibranceEffect::primaryOutputName() const
{
    return m_primaryOutputName;
}

void VibranceEffect::setPrimaryOutputName(const QString &name)
{
    if (name.size() > kMaxOutputNameLength) {
        qWarning() << "Way Vibrance: ignoring oversized primary output name" << name.size();
        return;
    }
    const QString trimmed = name.trimmed();
    if (trimmed == m_primaryOutputName) {
        return;
    }
    m_primaryOutputName = trimmed;
    Q_EMIT primaryOutputNameChanged(m_primaryOutputName);
    m_persistTimer->start();
    effects->addRepaintFull();
}

QStringList VibranceEffect::screenList() const
{
    QStringList out;
    const auto screens = effects->screens();
    out.reserve(screens.size());
    // Version-tolerant (auto): whatever the installed KWin calls its
    // outputs, the iteration compiles.
    for (const auto screen : screens) {
        QString model = QStringLiteral("%1 %2").arg(screen->manufacturer(), screen->model()).trimmed();
        if (model.isEmpty()) {
            model = QStringLiteral("output");
        }
        const QSize size = screen->geometry().size();
        out.append(QStringLiteral("%1|%2 — %3x%4")
                       .arg(screen->name(), model)
                       .arg(size.width())
                       .arg(size.height()));
    }
    return out;
}

void VibranceEffect::reloadConfig()
{
    auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    config->reparseConfiguration();
    KConfigGroup cfg(config, QStringLiteral("Effect-%1").arg(QString::fromLatin1(s_configName)));
    const qreal vibrance = cfg.readEntry("Vibrance", 1.25);
    const bool enabled = cfg.readEntry("Enabled", true);
    const bool primaryOnly = cfg.readEntry("PrimaryOnly", false);
    const QString primaryOutput = cfg.readEntry("PrimaryOutput", QString());
    const qreal red = cfg.readEntry("RedGain", 1.0);
    const qreal green = cfg.readEntry("GreenGain", 1.0);
    const qreal blue = cfg.readEntry("BlueGain", 1.0);

    m_vibrance = std::isfinite(vibrance) ? qBound<qreal>(0.0, vibrance, 2.0) : 1.0;
    m_enabled = enabled;
    m_primaryOnly = primaryOnly;
    m_primaryOutputName = primaryOutput.size() > kMaxOutputNameLength
        ? primaryOutput.left(kMaxOutputNameLength)
        : primaryOutput;
    m_rgbGains = clampGains(red, green, blue);

    m_rules.clear();
    const KConfigGroup rulesCfg = cfg.group(QString::fromLatin1(s_rulesGroupName));
    const QMap<QString, QString> entries = rulesCfg.entryMap();
    // entryMap() is key-sorted; rules are first-match-wins, so replaying the
    // persisted creation order first is what keeps a profile's priority
    // stable across a KWin restart. Entries the order does not mention (old
    // configs, hand edits) follow in map order.
    QStringList keys;
    const QStringList order =
        cfg.readEntry(QString::fromLatin1(s_rulesOrderKey), QStringList());
    for (const QString &key : order) {
        if (!keys.contains(key)) {
            keys.append(key);
        }
    }
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (!keys.contains(it.key())) {
            keys.append(it.key());
        }
    }
    for (const QString &key : keys) {
        const auto entry = entries.constFind(key);
        if (entry == entries.constEnd()) {
            continue; // stale order entry
        }
        if (m_rules.size() >= kMaxAppRules) {
            qWarning() << "Way Vibrance: config profile list truncated at" << kMaxAppRules;
            break;
        }
        // Validate key length and characters
        const QString decodedKey = QUrl::fromPercentEncoding(entry.key().toLatin1());
        if (decodedKey.isEmpty() || decodedKey.size() > kMaxPrefixedPatternLength || decodedKey.contains(QLatin1Char('|'))) {
            qWarning() << "Way Vibrance: invalid rule key" << entry.key();
            continue;
        }
        // Validate value length
        if (entry.value().size() > 512) {
            qWarning() << "Way Vibrance: rule value too long" << entry.value();
            continue;
        }
        AppRule rule;
        if (!parseRule(decodedKey + QLatin1Char('|') + entry.value(), rule)) {
            qWarning() << "Way Vibrance: failed to parse rule" << decodedKey << entry.value();
            continue;
        }
        // Validate vibrance range
        qreal valid = 1.0;
        if (!validFactor(rule.vibrance, valid)) {
            qWarning() << "Way Vibrance: invalid vibrance value in rule" << decodedKey;
            continue;
        }
        rule.vibrance = valid;
        // Validate gains range
        rule.gains = clampGains(rule.gains.x(), rule.gains.y(), rule.gains.z());
        // Validate pattern length. parseRule() already rejected embedded
        // separators by constructing fields from a '|' split, so no pipe
        // check is needed here.
        if (rule.pattern.size() > kMaxPatternLength) {
            qWarning() << "Way Vibrance: invalid pattern in rule" << decodedKey;
            continue;
        }
        // Validate displayName length
        if (rule.displayName.size() > kMaxNameLength) {
            rule.displayName = rule.displayName.left(kMaxNameLength);
        }
        m_rules.append(rule);
    }
    invalidateMatchCache();

    updateShaderUniforms();
    updateRedirects();

    Q_EMIT vibranceChanged(m_vibrance);
    Q_EMIT enabledChanged(m_enabled);
    Q_EMIT rgbGainsChanged(m_rgbGains.x(), m_rgbGains.y(), m_rgbGains.z());
    Q_EMIT primaryOnlyChanged(m_primaryOnly);
    Q_EMIT primaryOutputNameChanged(m_primaryOutputName);
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
        // Names are guaranteed pipe-free at every entry point (applyRule /
        // setAppRuleName strip '|'), so no escaping is needed — and escaping
        // here would be ineffective anyway, because parseRule splits on '|'
        // before it could unescape the field.
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
    // Non-finite channels degrade to neutral (1.0) rather than poisoning
    // the shader uniform.
    auto channel = [](qreal v) {
        return std::isfinite(v) ? static_cast<float>(qBound(0.0, v, 2.0)) : 1.0f;
    };
    return QVector3D(channel(red), channel(green), channel(blue));
}


void VibranceEffect::invalidateMatchCache()
{
    // Bump only: entries keep their appliedShader bookkeeping, which
    // drawWindow diffs against the (re-resolved) desired state.
    ++m_matchStamp;
}

void VibranceEffect::applyRule(const QString &prefixedPattern, qreal vibrance,
                               const std::optional<QVector3D> &gains,
                               const std::optional<QString> &name)
{
    // Bound the raw input BEFORE trimmed()/mid() touch it: a session peer
    // must not be able to charge the compositor for megabyte string ops.
    if (prefixedPattern.size() > kMaxPrefixedPatternLength) {
        qWarning() << "Way Vibrance: app profile input too long" << prefixedPattern.size();
        return;
    }
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
    if (pattern.size() > kMaxPatternLength) {
        qWarning() << "Way Vibrance: app profile pattern too long" << pattern.size();
        return;
    }
    qreal validVibrance = 1.0;
    if (!validFactor(vibrance, validVibrance)) {
        qWarning() << "Way Vibrance: ignoring non-finite profile vibrance";
        return;
    }
    vibrance = validVibrance;
    if (gains.has_value()
        && !(std::isfinite(gains->x()) && std::isfinite(gains->y()) && std::isfinite(gains->z()))) {
        qWarning() << "Way Vibrance: ignoring non-finite profile gains";
        return;
    }
    QString cleanName;
    if (name.has_value()) {
        cleanName = (name->size() > kMaxNameLength) ? QString(name->left(kMaxNameLength)) : *name;
        cleanName = cleanName.trimmed();
        cleanName.remove(QLatin1Char('|'));
        if (cleanName.size() > kMaxNameLength) {
            cleanName = cleanName.left(kMaxNameLength);
        }
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
        if (m_rules.size() >= kMaxAppRules) {
            qWarning() << "Way Vibrance: profile limit reached" << kMaxAppRules
                       << "- remove one before adding another";
            return;
        }
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

    invalidateMatchCache();
    updateRedirects();
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
    // applyRule() rejects non-finite gains; validate before clampGains(),
    // which deliberately degrades non-finite channels to neutral for the
    // global path.
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue)) {
        qWarning() << "Way Vibrance: ignoring non-finite profile gains";
        return;
    }
    applyRule(prefixedPattern, vibrance, clampGains(red, green, blue), std::nullopt);
}

void VibranceEffect::setAppRuleName(const QString &prefixedPattern, const QString &name)
{
    // Cosmetic-only edit: finds the existing rule and touches nothing else.
    if (prefixedPattern.size() > kMaxPrefixedPatternLength) {
        qWarning() << "Way Vibrance: app profile input too long" << prefixedPattern.size();
        return;
    }
    bool titleMatch = false;
    QString pattern = prefixedPattern.trimmed();
    if (pattern.startsWith(QLatin1String("title:"))) {
        titleMatch = true;
        pattern = pattern.mid(6).trimmed();
    } else if (pattern.startsWith(QLatin1String("app:"))) {
        pattern = pattern.mid(4).trimmed();
    }
    // Cap before trim: QString::left() bounds every later pass over the
    // string; a name longer than the display cap is truncated (as before),
    // just without ever processing the excess.
    QString cleanName = (name.size() > kMaxNameLength) ? name.left(kMaxNameLength) : name;
    cleanName = cleanName.trimmed();
    cleanName.remove(QLatin1Char('|'));
    if (cleanName.size() > kMaxNameLength) {
        cleanName = cleanName.left(kMaxNameLength);
    }
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
    if (prefixedPattern.size() > kMaxPrefixedPatternLength) {
        return; // nothing legitimate (or addable) is this long
    }
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
    return m_lastFocusedAppId.size() > kMaxWindowIdentityLength
        ? m_lastFocusedAppId.left(kMaxWindowIdentityLength)
        : m_lastFocusedAppId;
}

/**
 * Refreshes the cached per-window rule resolution (first match wins). Cheap:
 * recomputes only when the window's identity strings changed or the rule set
 * was touched (stamp). appliedShader survives — drawWindow diffs it.
 */
void VibranceEffect::resolveWindow(EffectWindow *w)
{
    WindowMatch &entry = m_windowMatch[w];
    static const QString s_noString;
    const QString &rawClass = w->window() ? w->window()->resourceClass() : s_noString;
    const QString &rawName = w->window() ? w->window()->resourceName() : s_noString;
    const QString &rawCaption = w->caption();
    // Cap before anything touches these (compare/match/store): a window
    // claiming megabyte identities costs this path at most kMaxWindow-
    // IdentityLength per frame and is excluded from profile matching —
    // an oversized identity is not a style anyone can name with a glob.
    const bool oversize = rawClass.size() > kMaxWindowIdentityLength
        || rawName.size() > kMaxWindowIdentityLength
        || rawCaption.size() > kMaxWindowIdentityLength;
    if (oversize) {
        // Oversized identities never match any profile (policy), so the
        // only state that matters is the flag + stamp. A window that keeps
        // re-publishing megabyte names hits this return instead of paying
        // three 1 KB truncation copies and a memcmp per frame.
        if (entry.oversize && entry.stamp == m_matchStamp) {
            return;
        }
        entry.stamp = m_matchStamp;
        entry.oversize = true;
        entry.idA.clear();
        entry.idB.clear();
        entry.caption.clear();
        entry.ruleIndex = -1;
        return;
    }
    if (entry.stamp == m_matchStamp && !entry.oversize && entry.idA == rawClass && entry.idB == rawName && entry.caption == rawCaption) {
        return;
    }
    entry.stamp = m_matchStamp;
    entry.oversize = false;
    entry.idA = rawClass;
    entry.idB = rawName;
    entry.caption = rawCaption;
    entry.ruleIndex = -1;
    if (m_rules.isEmpty()) {
        return;
    }
    int budget = kResolveStepBudget; // aggregate across every rule of this pass
    for (int i = 0; i < m_rules.size(); ++i) {
        const AppRule &rule = m_rules.at(i);
        if (rule.pattern.isEmpty()) {
            continue;
        }
        if (rule.titleMatch) {
            if (globMatch(rule.pattern, rawCaption, &budget)) {
                entry.ruleIndex = i;
                break;
            }
        } else if ((globMatch(rule.pattern, rawClass, &budget) && !rawClass.isEmpty())
                   || (globMatch(rule.pattern, rawName, &budget) && !rawName.isEmpty())) {
            entry.ruleIndex = i;
            break;
        }
    }
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
    // Cap before ANY scan of it: contains() on a hostile megabyte identity
    // would charge the compositor per activation (resolveWindow's cap does
    // not cover this path — this is the only consumer of the raw id).
    if (id.size() > kMaxWindowIdentityLength) {
        id.truncate(kMaxWindowIdentityLength);
    }
    // Our own windows must not shadow the game/app the user just alt+tabbed
    // through on the way to the tray controls.
    if (id.isEmpty() || id.contains(QLatin1String("wayvibrance"), Qt::CaseInsensitive)) {
        return;
    }
    m_lastFocusedAppId = std::move(id);
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
    if (m_shaderGaveUp) {
        return false; // terminal: warn once below, never from the paint path
    }
    if (m_shaderFailedAttempts > 0 && m_shaderRetryTimer.isValid()
        && m_shaderRetryTimer.elapsed() < kShaderRetryDelayMs) {
        return false; // cooldown between compile attempts
    }
    m_shader = ShaderManager::instance()->generateShaderFromFile(ShaderTrait::MapTexture, QString(),
                                                                 QLatin1String(s_shaderPath));
    if (!m_shader) {
        ++m_shaderFailedAttempts;
        m_shaderRetryTimer.start();
        if (m_shaderFailedAttempts >= kMaxShaderAttempts) {
            m_shaderGaveUp = true;
            qWarning() << "Way Vibrance: failed to load the RGB gain shader" << QLatin1String(s_shaderPath)
                       << "after" << kMaxShaderAttempts
                       << "attempts - per-channel gains disabled until KWin reloads the effect.";
        } else {
            qWarning() << "Way Vibrance: failed to load the RGB gain shader" << QLatin1String(s_shaderPath)
                       << "- attempt" << m_shaderFailedAttempts << "of" << kMaxShaderAttempts
                       << "; retrying.";
        }
        return false;
    }
    m_shaderFailedAttempts = 0;
    m_shaderRetryTimer.invalidate();
    ShaderBinder binder{m_shader.get()};
    m_shader->setUniform("rgbGain", m_rgbGains);
    return true;
}

void VibranceEffect::updateShaderUniforms()
{
    // Applied lazily in drawWindow, where the GL context is current.
    m_uniformsDirty = true;
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
        return it.value().shader.get();
    }

    // Cache full: first recycle the variants no window is using any more
    // (typical case — repeated edits of the same profile's gains). The
    // in-use check is mandatory: WindowMatch keeps raw pointers to what it
    // applied, and we are only ever called with a GL context current
    // (drawWindow), so freeing here is otherwise safe.
    if (m_shaderPool.size() >= kMaxShaderVariants) {
        for (auto pit = m_shaderPool.begin(); pit != m_shaderPool.end();) {
            const GLShader *candidate = pit.value().shader.get();
            const bool inUse = std::any_of(m_windowMatch.cbegin(), m_windowMatch.cend(),
                                           [candidate](const WindowMatch &m) {
                                               return m.appliedShader == candidate;
                                           });
            if (!inUse) {
                pit = m_shaderPool.erase(pit);
            } else {
                ++pit;
            }
        }
    }
    // Every slot is on screen (legitimate heavy use): substitute the
    // nearest cached variant only within the imperceptible threshold;
    // otherwise degrade explicitly (the caller unredirects and the window
    // keeps only the vibrance uniform) instead of silently recoloring it.
    if (m_shaderPool.size() >= kMaxShaderVariants) {
        GLShader *nearest = nullptr;
        float best = std::numeric_limits<float>::max();
        for (const ShaderVariant &variant : m_shaderPool) {
            const QVector3D d = variant.gains - gains;
            const float m = std::max({std::abs(d.x()), std::abs(d.y()), std::abs(d.z())});
            if (m < best) {
                best = m;
                nearest = variant.shader.get();
            }
        }
        if (nearest && best <= kShaderNearestMaxDelta) {
            return nearest;
        }
        if (!m_poolSaturatedWarned) {
            m_poolSaturatedWarned = true;
            qWarning().noquote() << "Way Vibrance: shader cache saturated (" << kMaxShaderVariants
                                 << "distinct gain sets on screen) — profiles needing farther gains fall back"
                                 << "to the vibrance-only path until some variant can be recycled";
        }
        return nullptr;
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
    m_shaderPool.insert(key, ShaderVariant{std::move(fresh), gains});
    m_poolSaturatedWarned = false; // room exists again: a later saturation gets one fresh notice
    return raw;
}

void VibranceEffect::unredirectAllApplied()
{
    for (auto it = m_windowMatch.begin(); it != m_windowMatch.end(); ++it) {
        if (it->appliedShader) {
            unredirect(it.key());
            it->appliedShader = nullptr;
        }
    }
}

void VibranceEffect::updateRedirects()
{
    // Redirects themselves are applied lazily inside drawWindow (the only
    // guaranteed in-context place). Here we force a full frame so every
    // visible window gets the chance to reconcile, and drop all grabs when
    // the effect is disabled — inactive effects never see drawWindow().
    // If the new state leaves the effect inactive (disabled, or neutral
    // globals with no profiles), KWin may stop calling drawWindow() —
    // release any grabs now, before reconciliation that would happen in a
    // paint that never comes.
    if (!isActive()) {
        unredirectAllApplied();
    }
    effects->addRepaintFull();
}

void VibranceEffect::setVibrance(qreal value)
{
    qreal valid = 1.0;
    if (!validFactor(value, valid)) {
        qWarning() << "Way Vibrance: ignoring non-finite vibrance";
        return;
    }
    value = valid;
    if (qFuzzyCompare(value, m_vibrance)) {
        return;
    }
    m_vibrance = value;
    Q_EMIT vibranceChanged(m_vibrance);
    m_persistTimer->start();
    // Goes through the redirect reconciliation: neutralizing the last
    // non-default global may turn the effect inactive, and an inactive
    // effect gets no drawWindow() call to release its grabs itself.
    updateRedirects();
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
    const QVector3D gains = clampGains(red, green, blue);
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
    cfg.writeEntry("PrimaryOutput", m_primaryOutputName);
    cfg.writeEntry("RedGain", static_cast<qreal>(m_rgbGains.x()));
    cfg.writeEntry("GreenGain", static_cast<qreal>(m_rgbGains.y()));
    cfg.writeEntry("BlueGain", static_cast<qreal>(m_rgbGains.z()));

    // Per-app profiles live in a child group; keys are the percent-encoded
    // "kind:pattern", values "vibrance" plus "|r,g,b" when the profile
    // carries its own channel gains.
    cfg.group(QString::fromLatin1(s_rulesGroupName)).deleteGroup();
    QStringList order;
    if (!m_rules.isEmpty()) {
        KConfigGroup rulesCfg(config, QStringLiteral("Effect-%1").arg(QString::fromLatin1(s_configName)));
        rulesCfg = rulesCfg.group(QString::fromLatin1(s_rulesGroupName));
        order.reserve(m_rules.size());
        for (const AppRule &rule : m_rules) {
            const QString id = serializedRule(rule);
            const int firstSep = id.indexOf(QLatin1Char('|'));
            const QString key = QString::fromLatin1(QUrl::toPercentEncoding(id.left(firstSep)));
            rulesCfg.writeEntry(key, id.mid(firstSep + 1));
            order.append(key);
        }
    }
    // Rules are first-match-wins and KConfig stores them key-sorted, so save
    // the creation order explicitly (in the parent group, never mixed with
    // the rule keys). Older builds ignore the extra key.
    if (order.isEmpty()) {
        cfg.deleteEntry(QString::fromLatin1(s_rulesOrderKey));
    } else {
        cfg.writeEntry(QString::fromLatin1(s_rulesOrderKey), order);
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
    QObject::connect(m_effect, &VibranceEffect::primaryOutputNameChanged, this,
                     &WayVibranceAdaptor::primaryOutputNameChanged);
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

QString WayVibranceAdaptor::primaryOutputName() const
{
    return m_effect->primaryOutputName();
}

void WayVibranceAdaptor::setPrimaryOutputName(const QString &name)
{
    m_effect->setPrimaryOutputName(name);
}

QStringList WayVibranceAdaptor::screenList() const
{
    return m_effect->screenList();
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
