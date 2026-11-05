/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <effect/effect.h>
#include <effect/effecthandler.h>

#include <QDBusAbstractAdaptor>

class QTimer;

namespace KWin
{

/**
 * A desktop effect that applies a saturation/vibrance boost to every window
 * being composited, the Wayland equivalent of NVIDIA Digital Vibrance or
 * AMD's Saturation setting on Windows.
 *
 * The boost is applied through WindowPaintData::multiplySaturation(), which
 * KWin's scene renderer handles with its built-in shader, in linear color
 * space, without grabbing windows into extra textures or adding a separate
 * fullscreen pass.
 *
 * The value is configurable at runtime through the D-Bus service
 * "org.wayvibrance" (object path "/Vibrance", interface "org.wayvibrance")
 * and persisted to kwinrc, so it is restored on every session.
 *
 * \sa WayVibranceAdaptor
 */
class VibranceEffect : public Effect
{
    Q_OBJECT

public:
    explicit VibranceEffect();
    ~VibranceEffect() override;

    static bool supported();

    bool isActive() const override;
    void reconfigure(ReconfigureFlags flags) override;
    bool drawWindow(const RenderTarget &renderTarget,
                    const RenderViewport &viewport,
                    EffectWindow *w,
                    int mask,
                    const Region &region,
                    WindowPaintData &data) override;

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
     * the configured vibrance value for when it is turned back on.
     */
    void setVibranceEnabled(bool enabled);
    bool vibranceEnabled() const;

Q_SIGNALS:
    void vibranceChanged(qreal value);
    void enabledChanged(bool enabled);

private Q_SLOTS:
    void persist();

private:
    void reloadConfig();

    qreal m_vibrance = 1.25;
    bool m_enabled = true;
    bool m_dbusRegistered = false;
    QTimer *m_persistTimer = nullptr;
};

/**
 * D-Bus adaptor exposing VibranceEffect on the session bus.
 *
 * Service: org.wayvibrance
 * Path:    /Vibrance
 * Interface: org.wayvibrance
 *
 * Methods: vibrance() -> d ; setVibrance(d) ; enabled() -> b ; setEnabled(b)
 * Signals: vibranceChanged(d) ; enabledChanged(b)
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

Q_SIGNALS:
    void vibranceChanged(qreal value);
    void enabledChanged(bool enabled);

private:
    VibranceEffect *m_effect;
};

} // namespace KWin
