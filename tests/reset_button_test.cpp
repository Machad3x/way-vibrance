/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Versioned guarantee for the circular "reset to default" button that sits
    next to each slider percentage (tray/resetbutton_p.h — the exact header
    the tray window and the profile dialog compile):

      1. clicking restores the default value through the normal QSlider
         signal path (which is what refreshes the label / marks the dialog
         dirty / pushes to the effect);
      2. clicking while already at the default emits nothing (no redundant
         D-Bus traffic);
      3. a default outside the slider range clamps to the nearest edge
         instead of being ignored;
      4. the visual contract (18x18, circular glyph, tooltip naming the
         value, accessible name) is intact, and the button is styled by the
         shared application sheet rather than an inline stylesheet, so a KDE
         colour-scheme switch re-polishes it together with every widget;
      5. the adjacent percentage label is right-aligned and has a fixed
         width for the slider's maximum, so rows with different values keep
         the percentage digits and the reset buttons in one column.

    Runs offscreen, no session bus needed. Exit code != 0 on failure.
*/

#include "resetbutton_p.h"
#include "style_p.h"

#include <QApplication>
#include <QSlider>
#include <QToolButton>

#include <cstdio>

static int s_total = 0;
static int s_failed = 0;

static void check(bool ok, const char *what)
{
    ++s_total;
    if (!ok) {
        fprintf(stderr, "RESET-BUTTON-FAIL %s\n", what);
        ++s_failed;
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen"); // headless: CI has no display
    QApplication app(argc, argv);
    app.setStyleSheet(wvstyle::buildAppStyle(app.palette()));

    // ---- 1. clicking restores the default, exactly one signal ----
    {
        QWidget parent;
        QSlider slider(&parent);
        slider.setRange(0, 200);
        slider.setValue(137);
        int changes = 0;
        int lastValue = -1;
        QObject::connect(&slider, &QSlider::valueChanged, [&](int value) {
            ++changes;
            lastValue = value;
        });

        QToolButton *reset = createResetButton(&parent, &slider, 100);
        check(reset != nullptr, "button created");
        check(reset->isEnabled(), "button enabled");
        check(reset->toolTip() == QStringLiteral("Reset to default (100%)"), "tooltip names default");
        check(!reset->accessibleName().isEmpty(), "accessible name present");
        check(reset->text() == QStringLiteral("\u21BA"), "circular reset glyph");
        check(reset->size() == QSize(18, 18), "fixed 18x18 (circle) size");
        check(reset->property("wvReset").toBool(), "marked for the application sheet");
        check(reset->styleSheet().isEmpty(), "no inline stylesheet (single theme source)");
        check(app.styleSheet().contains(QStringLiteral("border-radius: 9px")),
              "border radius = half the size (circle)");

        reset->click();
        check(slider.value() == 100, "click restores default");
        check(changes == 1, "exactly one valueChanged");
        check(lastValue == 100, "signal carried the default");

        // ---- 2. already at default: no spurious signal ----
        reset->click();
        check(slider.value() == 100, "value stays at default");
        check(changes == 1, "no spurious valueChanged at default");
    }

    // ---- 3. out-of-range defaults clamp to the slider edges ----
    {
        QWidget parent;
        QSlider slider(&parent);
        slider.setRange(0, 200);
        slider.setValue(50);

        QToolButton *high = createResetButton(&parent, &slider, 500);
        high->click();
        check(slider.value() == 200, "high default clamps to maximum");

        QToolButton *low = createResetButton(&parent, &slider, -40);
        low->click();
        check(slider.value() == 0, "low default clamps to minimum");
    }

    // ---- 4. non-100 defaults (dialog can reuse the same helper) ----
    {
        QWidget parent;
        QSlider slider(&parent);
        slider.setRange(0, 200);
        slider.setValue(25);

        QToolButton *reset = createResetButton(&parent, &slider, 150);
        check(reset->toolTip() == QStringLiteral("Reset to default (150%)"), "custom default in tooltip");
        reset->click();
        check(slider.value() == 150, "custom default restored");
    }

    // ---- 5. percentage labels keep the trailing button column stable ----
    {
        QWidget parent;
        QLabel *wide = createSliderValueLabel(&parent, 200);
        QLabel *shortValue = createSliderValueLabel(&parent, 200);
        check(wide->alignment().testFlag(Qt::AlignRight), "percentage right-aligned");
        check(wide->alignment().testFlag(Qt::AlignVCenter), "percentage vertically centred");
        wide->setText(QStringLiteral("200%"));
        shortValue->setText(QStringLiteral("5%"));
        check(wide->width() == shortValue->width(),
              "same column width regardless of the displayed value");
        check(wide->width() > wide->fontMetrics().horizontalAdvance(QStringLiteral("200%")),
              "column width fits the widest value");
        check(wide->minimumWidth() == wide->maximumWidth(),
              "fixed width (not a growing label) is what aligns the rows");
    }

    fprintf(stderr, "reset_button_test: total=%d failures=%d\n", s_total, s_failed);
    return s_failed ? 1 : 0;
}
