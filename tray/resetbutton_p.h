/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Small circular "reset to default" button shown to the right of a
    slider's percentage label, plus the label itself. Shared by the tray
    window and the per-app profile dialog so both stay visually and
    behaviorally identical; the click semantics are covered by
    tests/reset_button_test.cpp.
*/

#pragma once

#include <QLabel>
#include <QObject>
#include <QSlider>
#include <QStringLiteral>
#include <QToolButton>
#include <QWidget>

/**
 * Creates the percentage label that sits between a slider and its reset
 * button.
 *
 * The label is right-aligned and wide enough for the largest value the
 * slider can show. Right alignment keeps the digits on the same edge, and
 * the fixed width keeps the reset button in the same column even when one
 * row reads "80%" and another reads "137%" — both are values in this UI
 * (RGB gains and per-app vibrance).
 */
inline QLabel *createSliderValueLabel(QWidget *parent, int maximum = 200)
{
    auto *label = new QLabel(parent);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Reserve the widest text plus a little trailing breathing room. Fixed
    // width (not minimum) is what makes every row's trailing edge identical.
    label->setFixedWidth(label->fontMetrics().horizontalAdvance(
                             QStringLiteral("%1%").arg(maximum))
                         + 12);
    return label;
}

/**
 * Creates an 18x18 circular tool button that resets @p slider to
 * @p defaultValue when clicked.
 *
 * The value is clamped to the slider's own range, so a default that no
 * longer fits (should a range ever change) degrades to the nearest edge
 * instead of being silently ignored. Setting the value goes through the
 * normal QSlider::valueChanged path, which is what updates the percentage
 * label, marks the profile dialog dirty and pushes the change to the
 * effect — the button therefore needs no wiring of its own beyond this.
 *
 * Styling comes from the application sheet (tray/style_p.h) through the
 * "wvReset" property, not from an inline setStyleSheet(): that way a KDE
 * colour-scheme change re-polishes the button together with every other
 * widget instead of freezing the palette it was created with.
 */
inline QToolButton *createResetButton(QWidget *parent, QSlider *slider, int defaultValue = 100)
{
    auto *button = new QToolButton(parent);
    button->setText(QStringLiteral("\u21BA")); // ↺
    button->setToolTip(QObject::tr("Reset to default (%1%)").arg(defaultValue));
    button->setAccessibleName(QObject::tr("Reset slider to default"));
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    button->setCursor(Qt::PointingHandCursor);
    // Fixed 18px + 9px radius = a circle (the icon stays centered).
    button->setFixedSize(18, 18);
    // Selector for the application stylesheet; set before the first polish.
    button->setProperty("wvReset", true);

    QObject::connect(button, &QToolButton::clicked, slider, [slider, defaultValue] {
        slider->setValue(qBound(slider->minimum(), defaultValue, slider->maximum()));
    });
    return button;
}
