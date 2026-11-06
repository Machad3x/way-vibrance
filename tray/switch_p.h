/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Theme-aware on/off switch for the tray window.

    QSS cannot animate or paint a switch: QCheckBox::indicator needs bitmaps
    per state and does not animate, and a QML/Kirigami rewrite is out of
    scope. This widget therefore subclasses QCheckBox (keeping its checkable
    semantics, keyboard handling and accessibility role) and paints a label
    plus a switch track itself.

    Design constraints:

      * every colour comes from the live QPalette — no hard-coded theme;
      * no animations and no timers, so there is no repaint loop and reduced
        motion is respected by construction;
      * the whole row is a hit target, not just the track;
      * Space (inherited) and Enter/Return toggle it; focus is a visible
        WindowText ring that does not change the widget's size;
      * the knob mirrors in right-to-left layouts.

    The text is still stored in QCheckBox::text(), so screen readers and
    translations see exactly the same string as before.
*/

#pragma once

#include "style_p.h"

#include <QCheckBox>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QString>
#include <QWidget>

namespace wvswitch {

class Switch : public QCheckBox
{
public:
    explicit Switch(const QString &text = QString(), QWidget *parent = nullptr)
        : QCheckBox(text, parent)
    {
        setCursor(Qt::PointingHandCursor);
        // Full-width row: the layout stretches it, while the size hint
        // reserves room for the label and the track.
        setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
    }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(fontMetrics());
        return QSize(metrics.horizontalAdvance(text()) + kGap + kTrackWidth
                         + 2 * kEdge + kTextInset,
                     qMax(metrics.height(), kHeight));
    }

    QSize minimumSizeHint() const override
    {
        // Long translations may be elided instead of forcing the card wider.
        return QSize(kTrackWidth + kGap, kHeight);
    }

    /**
     * Track outline adapted until it reaches 3:1 against the card.
     *
     * Midlight is nearly the card colour in several real schemes (Breeze
     * Dark in particular), and Mid can be too, so the capsule would lose
     * its boundary and the knob would look detached. Blending toward the
     * foreground keeps the border neutral and tied to the live palette.
     */
    static QColor outlineColor(const QPalette &palette, QPalette::ColorGroup group,
                               const QColor &card)
    {
        const QColor base = palette.color(group, QPalette::Midlight);
        const QColor text = palette.color(group, QPalette::WindowText);
        QColor outline = base;
        for (int step = 1; step <= 20 && wvstyle::contrastRatio(outline, card) < 3.0; ++step) {
            outline = wvstyle::blend(base, text, step * 0.05);
        }
        return outline;
    }

    /**
     * Knob colour with a last-resort contrast guard.
     *
     * The preferred colour is the scheme foreground (off) or highlighted
     * text (on). A few high-contrast schemes set Mid or Highlight to the
     * same colour as that foreground, which would erase the knob entirely;
     * in that case the other semantic foregrounds are tried and, failing
     * that, black or white is chosen against the track fill. The fallback
     * never triggers on Breeze Light/Dark (checked by the test).
     */
    static QColor knobColor(const QPalette &palette, bool checked, QPalette::ColorGroup group)
    {
        const QColor fill = palette.color(group, checked ? QPalette::Highlight : QPalette::Mid);
        const QColor preferred = palette.color(
            QPalette::Active, checked ? QPalette::HighlightedText : QPalette::WindowText);
        if (wvstyle::contrastRatio(preferred, fill) >= 2.0) {
            return preferred;
        }

        const QColor candidates[] = {
            palette.color(QPalette::Active, QPalette::WindowText),
            palette.color(QPalette::Active, QPalette::HighlightedText),
            palette.color(QPalette::Active, QPalette::Text),
            palette.color(QPalette::Active, QPalette::Base),
        };
        QColor best = preferred;
        for (const QColor &candidate : candidates) {
            if (wvstyle::contrastRatio(candidate, fill)
                > wvstyle::contrastRatio(best, fill)) {
                best = candidate;
            }
        }
        if (wvstyle::contrastRatio(best, fill) < 2.0) {
            best = wvstyle::relativeLuminance(fill) > 0.5 ? QColor(Qt::black) : QColor(Qt::white);
        }
        return best;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
        const QPalette &pal = palette();
        const bool checked = isChecked();
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        // The cards keep the application's active surface even while one
        // control is disabled, so the contrast reference must not follow the
        // widget's current (Disabled) group.
        QPalette activePalette = pal;
        activePalette.setCurrentColorGroup(QPalette::Active);
        const QColor card = wvstyle::cardColor(activePalette);

        // The track sits at the trailing edge.
        const QRectF track(rtl ? kEdge : width() - kEdge - kTrackWidth,
                           (height() - kTrackHeight) / 2.0,
                           kTrackWidth, kTrackHeight);

        // Outline first: it keeps the component boundary visible even in
        // light schemes where Mid/Highlight are close to the card surface.
        // A disabled switch dims the track and the knob together, but the
        // knob keeps the active foreground colour: Disabled WindowText and
        // Mid often coincide, which would erase the knob and therefore the
        // on/off position.
        painter.setOpacity(isEnabled() ? 1.0 : 0.6);
        painter.setPen(QPen(outlineColor(pal, group, card), 1.0));
        painter.setBrush(pal.color(group, checked ? QPalette::Highlight : QPalette::Mid));
        painter.drawRoundedRect(track, kTrackHeight / 2.0, kTrackHeight / 2.0);

        // Knob: foreground colour when off, highlighted-text when on. The
        // position (leading vs trailing) carries the state as well, so the
        // switch stays legible even if a scheme has a low knob/track ratio.
        const qreal radius = kKnob / 2.0 - (isDown() ? 1.0 : 0.0);
        const bool knobTrailing = checked != rtl;
        const QPointF knobCenter(knobTrailing ? track.right() - kKnobInset - kKnob / 2.0
                                              : track.left() + kKnobInset + kKnob / 2.0,
                                 track.center().y());
        painter.setPen(Qt::NoPen);
        painter.setBrush(knobColor(pal, checked, group));
        painter.drawEllipse(knobCenter, radius, radius);
        painter.setOpacity(1.0);

        // Focus is a row-level ring: it is drawn inside the widget so the
        // switch can keep the height of a text row and never move the card.
        if (hasFocus() && isEnabled()) {
            // Not cosmetic: on a 2x display the ring must scale with the
            // device pixel ratio instead of staying 1.5 device pixels thin.
            QPen ring(pal.color(QPalette::Active, QPalette::WindowText), 1.5);
            painter.setPen(ring);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75),
                                    4.0, 4.0);
        }

        // Label, elided at the available width so a longer translation does
        // not push the switch out of the card.
        // A small inset keeps the label clear of the focus ring, which is
        // drawn on the row edge.
        const qreal textStart = kEdge + kTextInset;
        const QRectF textRect(rtl ? track.right() + kGap : textStart,
                              0.0,
                              qMax(0.0, rtl ? width() - track.right() - kGap - textStart
                                            : track.left() - kGap - textStart),
                              height());
        if (textRect.width() > 0.0 && !text().isEmpty()) {
            painter.setFont(font());
            painter.setPen(pal.color(group, QPalette::WindowText));
            const Qt::Alignment alignment =
                (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter;
            painter.drawText(textRect, alignment,
                             fontMetrics().elidedText(text(),
                                                      rtl ? Qt::ElideLeft : Qt::ElideRight,
                                                      int(textRect.width())));
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        // QCheckBox handles Space; Enter/Return are expected from a button
        // and cost nothing to support here.
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && isEnabled() && !event->isAutoRepeat()) {
            click();
            event->accept();
            return;
        }
        QCheckBox::keyPressEvent(event);
    }

    bool hitButton(const QPoint &pos) const override
    {
        // The label is part of the control: clicking anywhere on the row
        // toggles it, which is what the previous checkboxes did too.
        return rect().contains(pos);
    }

    void changeEvent(QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::EnabledChange:
            setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
            break;
        case QEvent::PaletteChange:
        case QEvent::LayoutDirectionChange:
            update();
            break;
        case QEvent::FontChange:
        case QEvent::StyleChange:
            updateGeometry();
            update();
            break;
        default:
            break;
        }
        QCheckBox::changeEvent(event);
    }

private:
    // One text row tall: the switch must not make the window taller than a
    // plain QCheckBox did, or the window stops fitting a 1366x768 work area.
    static constexpr int kHeight = 18;
    static constexpr int kTrackWidth = 34;
    static constexpr int kTrackHeight = 18;
    static constexpr int kKnob = 14;
    static constexpr int kKnobInset = 2;
    static constexpr int kGap = 12;
    static constexpr int kEdge = 2;
    static constexpr int kTextInset = 2;
};

} // namespace wvswitch
