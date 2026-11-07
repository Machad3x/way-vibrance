/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <effect/effect.h>
#include <effect/effecthandler.h>
#include <effect/offscreeneffect.h>

#include <QDBusAbstractAdaptor>
#include <QRegion>
#include "common_limits.h"
#include <QVector3D>

#include <memory>
#include <optional>

class QTimer;

namespace KWin
{

class GLShader;

// Version tolerance, part 1: the region parameter type of
// Effect::drawWindow() switched from QRegion to KWin::Region with the
// scene-renderer rework (KWin 6.6, effects API 0.237). We key on the API
// macro the headers themselves define — never on a distro/version guess —
// so the same source compiles on KWin 6.0 through 6.9+ unchanged.
#if !defined(KWIN_EFFECT_API_VERSION_MINOR) || KWIN_EFFECT_API_VERSION_MINOR < 236
#error "way-vibrance requires the KWin 6 effects API (KWin >= 6.0)"
#endif
#if KWIN_EFFECT_API_VERSION_MINOR >= 237
using WvPaintRegion = Region;
#else
using WvPaintRegion = QRegion;
#endif

// Version tolerance, part 2: Effect::drawWindow() gained a bool return
// value in KWin 6.8 (void on 6.0–6.7). Rather than guessing versions, the
// override below declares its return type as
// decltype(effects->drawWindow(…)) — EffectsHandler::drawWindow evolves in
// lockstep with the effect base (void+QRegion ≤6.5, void+Region 6.6/6.7,
// bool+Region 6.8+) and is public, so one definition compiles on 6.x with
// zero preprocessor version games.

/**
 * A desktop effect that applies a saturation/vibrance boost and per-channel
 * RGB gains to every window being composited — the Wayland equivalent of
 * NVIDIA's Digital Vibrance plus the per-channel color controls of the
 * driver panels.
 *
 * Two rendering paths, chosen by cost:
 *
 *  - Vibrance only (RGB gains neutral): the multiplier is handed to
 *    WindowPaintData::multiplySaturation(), which KWin's scene renderer
 *    applies with its built-in shader, in linear color space, without
 *    grabbing windows or adding any pass.
 *
 *  - RGB gains active: windows are additionally redirected through
 *    OffscreenEffect with a custom shader that multiplies the channels in
 *    linear space. The grab reuses the built-in saturation, so the vibrance
 *    keeps applying on this path too.
 *
 * While every value is neutral (1.0), no profile exists, or the effect is
 * disabled, isActive() returns false and KWin runs without touching this
 * effect at all.
 *
 * The values are configurable at runtime through the D-Bus service
 * "org.wayvibrance" (object path "/Vibrance", interface "org.wayvibrance")
 * and persisted to kwinrc, so they are restored on every session.
 *
 * \sa WayVibranceAdaptor
 */
class VibranceEffect : public OffscreenEffect
{
    Q_OBJECT

public:
    explicit VibranceEffect();
    ~VibranceEffect() override;

    static bool supported();

    bool isActive() const override;
    void reconfigure(ReconfigureFlags flags) override;

    auto drawWindow(const RenderTarget &renderTarget,
                    const RenderViewport &viewport,
                    EffectWindow *w,
                    int mask,
                    const WvPaintRegion &region,
                    WindowPaintData &data)
        -> decltype(effects->drawWindow(renderTarget, viewport, w, mask, region, data)) override;

    int requestedEffectChainPosition() const override
    {
        // Run as late as possible so the color grade applies over the final
        // appearance of every window, like the GPU driver's vibrance does.
        return 99;
    }

    /**
     * Sets the saturation multiplier. 1.0 keeps colors unchanged, values
     * below desaturate and values above boost. The value is clamped to
     * [0, 2] and persisted (debounced) to kwinrc. (2.0 is where KWin's
     * pipeline saturates — beyond it pixels clip without further boost,
     * the same ceiling as NVIDIA's 1023 Digital Vibrance value.)
     */
    void setVibrance(qreal value);
    qreal vibrance() const;

    /**
     * When disabled the effect stays loaded but behaves as neutral, keeping
     * the configured values for when it is turned back on.
     */
    void setVibranceEnabled(bool enabled);
    bool vibranceEnabled() const;

    /**
     * Flips the enabled flag on the server, so controllers (global hotkey)
     * never need a cached copy of the state — and therefore cannot get
     * stuck toggling "false to false" if a signal is missed.
     */
    void toggleVibranceEnabled();

    /**
     * Restricts the color grading to one fixed output — the "primary"
     * monitor the user picks in the tray (or the first output when unset;
     * KWin exposes no compositor-wide primary concept to effects, and
     * activeScreen() follows the pointer, which is not what people mean).
     * Out-of-scope windows are composited untouched, keeping the zero-cost
     * uniform path for them.
     */
    void setPrimaryOnly(bool primaryOnly);
    bool primaryOnly() const;
    void setPrimaryOutputName(const QString &name);
    QString primaryOutputName() const;
    QStringList screenList() const; // "name|Manufacturer Model WxH"

    /**
     * Sets per-channel RGB gain multipliers (0 = black, 1 = unchanged,
     * >1 = boosted, clipping like the driver does). Clamped to [0, 2]
     * persisted to kwinrc. While any channel differs from 1.0, windows are
     * redirected through the offscreen shader path.
     */
    void setRGBGains(qreal red, qreal green, qreal blue);

    qreal redGain() const;
    qreal greenGain() const;
    qreal blueGain() const;

    /**
     * Per-app profiles: vibrance overrides keyed by a glob pattern, the
     * counterpart of vibrance-gui's per-game settings.
     *
     * A pattern is "app:<glob>" (default when no prefix is given) and is
     * matched case-insensitively against the window's resource identifiers —
     * the Wayland app_id or the X11 WM_CLASS — or "title:<glob>", matched
     * against the window caption. '*' and '?' are the wildcards.
     *
     * Windows matching a profile are composited with the profile's
     * vibrance (and, when set, its own per-channel RGB gains) instead of
     * the global values; every other window keeps using the global ones.
     * Vibrance rides the same built-in saturation uniform as the global
     * value, so it costs nothing beyond a per-window lookup; profile RGB
     * gains use one GLShader instance per distinct gain set — redirected
     * windows already grab through the offscreen pass, so a profile only
     * compiles a shader the first time its exact gains appear.
     *
     * The serialized form used by D-Bus and kwinrc is
     * "<app|title>:<pattern>|<vibrance>" plus "|<r>,<g>,<b>" when the
     * profile carries its own channel gains.
     */
    void setAppRule(const QString &prefixedPattern, qreal vibrance);
    void setAppRuleFull(const QString &prefixedPattern, qreal vibrance,
                        qreal red, qreal green, qreal blue);
    void setAppRuleName(const QString &prefixedPattern, const QString &name);
    void removeAppRule(const QString &prefixedPattern);
    void clearAppRules();
    QStringList appRules() const;

    /**
     * Resource class of the most recently activated "real" window (normal,
     * not one of ours): lets controllers offer "add from last focused".
     */
    QString lastFocusedAppId() const;

Q_SIGNALS:
    void vibranceChanged(qreal value);
    void enabledChanged(bool enabled);
    void rgbGainsChanged(qreal red, qreal green, qreal blue);
    void primaryOnlyChanged(bool primaryOnly);
    void primaryOutputNameChanged(const QString &name);
    void appRulesChanged(const QStringList &rules);

private Q_SLOTS:
    void persist();
    void onWindowDeleted(KWin::EffectWindow *w);
    void onWindowActivated(KWin::EffectWindow *w);

private:
    struct AppRule
    {
        QString pattern; // glob without the kind prefix
        bool titleMatch = false;
        qreal vibrance = 1.0;
        QVector3D gains{1.0f, 1.0f, 1.0f}; // per-profile channel gains
        QString displayName; // cosmetic only: what the UI lists instead of
                             // the friendly form of the pattern (matching
                             // keeps using the pattern)
        bool hasGains() const { return !qFuzzyCompare(gains, QVector3D(1.0f, 1.0f, 1.0f)); }
    };

    struct WindowMatch
    {
        quint64 stamp = 0;
        bool oversize = false; // identity was capped: excluded from rule matching
        QString idA;
        QString idB;
        QString caption;
        int ruleIndex = -1; // index into m_rules; -1 = use the global values
        GLShader *appliedShader = nullptr; // offscreen shader we applied to
                                           // this window (null: not redirected
                                           // by us) — diffed against the
                                           // desired state inside drawWindow
    };

    void resolveWindow(EffectWindow *w);
    void invalidateMatchCache();
    static QString serializedRule(const AppRule &rule);
    static bool parseRule(const QString &serialized, AppRule &ruleOut);
    void applyRule(const QString &prefixedPattern, qreal vibrance,
                   const std::optional<QVector3D> &gains, const std::optional<QString> &name);

    void reloadConfig();
    bool rgbGainsActive() const;
    bool windowInScope(EffectWindow *w) const;
    bool ensureShader();
    void updateShaderUniforms();
    void updateRedirects();
    void unredirectAllApplied();
    GLShader *shaderForGains(const QVector3D &gains);
    static QString gainsKey(const QVector3D &gains);

    qreal m_vibrance = 1.25;
    bool m_enabled = true;
    bool m_primaryOnly = false;
    QString m_primaryOutputName; // empty = first output (automatic)
    QVector3D m_rgbGains{1.0f, 1.0f, 1.0f};
    bool m_uniformsDirty = false; // global rgbGain uniform re-upload happens
                                  // in drawWindow, where the GL context is
                                  // current (never from D-Bus/config events)
    QVector<AppRule> m_rules;
    QHash<EffectWindow *, WindowMatch> m_windowMatch;
    quint64 m_matchStamp = 0;
    QString m_lastFocusedAppId;
    struct ShaderVariant {
        std::shared_ptr<GLShader> shader;
        QVector3D gains; // kept for the bounded-cache nearest-variant path
    };
    QHash<QString, ShaderVariant> m_shaderPool; // per-gain-set shaders (capped)
    bool m_poolSaturatedWarned = false; // rate-limits the saturation warning
    bool m_shaderFailed = false;
    int m_shaderFailedAttempts = 0;
    bool m_dbusRegistered = false;
    QTimer *m_persistTimer = nullptr;
    std::unique_ptr<GLShader> m_shader;
};

/**
 * D-Bus adaptor exposing VibranceEffect on the session bus.
 *
 * Service: org.wayvibrance
 * Path:    /Vibrance
 * Interface: org.wayvibrance
 *
 * Methods: vibrance() -> d ; setVibrance(d) ; enabled() -> b ; setEnabled(b)
 *          toggleEnabled() ; primaryOnly() -> b ; setPrimaryOnly(b)
 *          primaryOutputName() -> s ; setPrimaryOutputName(s) ; screenList() -> as
 *          redGain()/greenGain()/blueGain() -> d ; setRGBGains(d, d, d)
 *          appRules() -> as ; setAppRule(s, d) ; setAppRuleFull(s, d, d, d, d)
 *          setAppRuleName(s, s) ; removeAppRule(s) ; clearAppRules() ;
 *          lastFocusedAppId() -> s
 * Signals: vibranceChanged(d) ; enabledChanged(b) ; rgbGainsChanged(d, d, d)
 *          primaryOnlyChanged(b) ; primaryOutputNameChanged(s) ; appRulesChanged(as)
 */
class WayVibranceAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.wayvibrance")

public:
    explicit WayVibranceAdaptor(VibranceEffect *effect);

public Q_SLOTS:
    qreal vibrance() const;
    void setVibrance(qreal value);
    bool enabled() const;
    void setEnabled(bool enabled);
    void toggleEnabled();
    bool primaryOnly() const;
    void setPrimaryOnly(bool primaryOnly);
    QString primaryOutputName() const;
    void setPrimaryOutputName(const QString &name);
    QStringList screenList() const;
    qreal redGain() const;
    qreal greenGain() const;
    qreal blueGain() const;
    void setRGBGains(qreal red, qreal green, qreal blue);
    QStringList appRules() const;
    void setAppRule(const QString &prefixedPattern, qreal vibrance);
    void setAppRuleFull(const QString &prefixedPattern, qreal vibrance,
                        qreal red, qreal green, qreal blue);
    void setAppRuleName(const QString &prefixedPattern, const QString &name);
    void removeAppRule(const QString &prefixedPattern);
    void clearAppRules();
    QString lastFocusedAppId() const;

Q_SIGNALS:
    void vibranceChanged(qreal value);
    void enabledChanged(bool enabled);
    void rgbGainsChanged(qreal red, qreal green, qreal blue);
    void primaryOnlyChanged(bool primaryOnly);
    void primaryOutputNameChanged(const QString &name);
    void appRulesChanged(const QStringList &rules);

private:
    VibranceEffect *m_effect;
};

} // namespace KWin
