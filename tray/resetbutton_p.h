/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Small circular "reset to default" button shown to the right of a
    slider's percentage label. Shared by the tray window and the per-app
    profile dialog so both stay visually and behaviorally identical; the
    click semantics are covered by tests/reset_button_test.cpp.
*/

#pragma once

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QSlider>
#include <QStringLiteral>
#include <QToolButton>
#include <QWidget>

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

    const QPalette pal = parent->palette();
    button->setStyleSheet(QStringLiteral(
        "QToolButton {"
        " border: 1px solid %1;"
        " border-radius: 9px;"
        " padding: 0px;"
        " font-size: 11px;"
        " color: %2;"
        " }"
        "QToolButton:hover { background: %3; }"
        "QToolButton:pressed { background: %1; }")
        .arg(pal.color(QPalette::Mid).name(),
             pal.color(QPalette::ButtonText).name(),
             pal.color(QPalette::Midlight).name()));

    QObject::connect(button, &QToolButton::clicked, slider, [slider, defaultValue] {
        slider->setValue(qBound(slider->minimum(), defaultValue, slider->maximum()));
    });
    return button;
}
