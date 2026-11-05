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
#include <QVector3D>

#include <memory>

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
 * While the values are neutral (1.0) or the effect is disabled, isActive()
 * returns false and KWin runs without touching this effect at all.
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
     * [0, 4] and persisted (debounced) to kwinrc.
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
     * Restricts the color grading to the active screen (the output holding
     * the pointer — KWin exposes no public "primary output" to effects).
     * Out-of-scope windows are composited untouched, keeping the zero-cost
     * uniform path for them.
     */
    void setPrimaryOnly(bool primaryOnly);
    bool primaryOnly() const;

    /**
     * Sets per-channel RGB gain multipliers (0 = black, 1 = unchanged,
     * >1 = boosted, clipping like the driver does). Clamped to [0, 4],
     * persisted to kwinrc. While any channel differs from 1.0, windows are
     * redirected through the offscreen shader path.
     */
    void setRGBGains(qreal red, qreal green, qreal blue);

    qreal redGain() const;
    qreal greenGain() const;
    qreal blueGain() const;

Q_SIGNALS:
    void vibranceChanged(qreal value);
    void enabledChanged(bool enabled);
    void rgbGainsChanged(qreal red, qreal green, qreal blue);
    void primaryOnlyChanged(bool primaryOnly);

private Q_SLOTS:
    void persist();
    void onWindowAdded(KWin::EffectWindow *w);

private:
    void reloadConfig();
    bool rgbGainsActive() const;
    bool windowInScope(EffectWindow *w) const;
    void ensureShader();
    void updateShaderUniforms();
    void updateRedirects();

    qreal m_vibrance = 1.25;
    bool m_enabled = true;
    bool m_primaryOnly = false;
    QVector3D m_rgbGains{1.0f, 1.0f, 1.0f};
    bool m_redirecting = false;
    bool m_shaderFailed = false;
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
 *          redGain()/greenGain()/blueGain() -> d ; setRGBGains(d, d, d)
 * Signals: vibranceChanged(d) ; enabledChanged(b) ; rgbGainsChanged(d, d, d)
 *          primaryOnlyChanged(b)
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
    qreal redGain() const;
    qreal greenGain() const;
    qreal blueGain() const;
    void setRGBGains(qreal red, qreal green, qreal blue);

Q_SIGNALS:
    void vibranceChanged(qreal value);
    void enabledChanged(bool enabled);
    void rgbGainsChanged(qreal red, qreal green, qreal blue);
    void primaryOnlyChanged(bool primaryOnly);

private:
    VibranceEffect *m_effect;
};

} // namespace KWin
