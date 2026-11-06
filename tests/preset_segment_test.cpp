/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Versioned guarantees for the segmented preset control
    (tray/segmented_p.h — the exact header the tray window and the per-app
    dialog compile):

      1. every segment is checkable, marked for the application sheet and
         the outer ones carry the wvSegFirst/wvSegLast properties (QSS has
         no :first/:last);
      2. exactly one segment is checked for a preset value, including after
         a click that does not change the slider value;
      3. no segment is checked for a non-preset slider value — the case Qt's
         exclusive QButtonGroup refuses to clear on its own;
      4. the group's exclusivity is restored after every sync, so a later
         click still behaves like a segmented control;
      5. the factory tolerates a null group (the tray can call sync before
         the control is built);
      6. keyboard focus stays visible on a checked segment without changing
         its size (the checked border and a highlight focus border would be
         the same colour, hiding the ring);
      7. the shared preset set is the documented 0-200% by 25 steps and
         every segment expands, so a row spans its card at equal widths.

    Runs offscreen, no session bus needed. Exit code != 0 on failure.
*/

#include "segmented_p.h"
#include "style_p.h"

#include <QApplication>
#include <QButtonGroup>
#include <QImage>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

static int s_total = 0;
static int s_failed = 0;

static void check(bool ok, const char *what)
{
    ++s_total;
    if (!ok) {
        fprintf(stderr, "PRESET-SEGMENT-FAIL %s\n", what);
        ++s_failed;
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setStyleSheet(wvstyle::buildAppStyle(app.palette()));

    // ---- 1. factory contract ----
    {
        QWidget parent;
        QPushButton *firstPaint = wvseg::createPresetButton(&parent, QStringLiteral("50%"), true, false);
        QPushButton *middlePaint = wvseg::createPresetButton(&parent, QStringLiteral("100%"), false, false);
        QPushButton *lastPaint = wvseg::createPresetButton(&parent, QStringLiteral("200%"), false, true);

        check(firstPaint->isCheckable(), "segments are checkable");
        check(firstPaint->property("wvPreset").toBool(), "segment marked for the sheet");
        check(firstPaint->property("wvSegFirst").toBool(), "first edge property");
        check(!middlePaint->property("wvSegFirst").toBool(), "middle has no first edge");
        check(!middlePaint->property("wvSegLast").toBool(), "middle has no last edge");
        check(lastPaint->property("wvSegLast").toBool(), "last edge property");
        check(!firstPaint->isChecked(), "factory leaves selection to sync");

        delete firstPaint;
        delete middlePaint;
        delete lastPaint;
    }

    // ---- 2-4. selection semantics against a real group ----
    // (built from the shared preset set the tray, dialog and menu use;
    //  indices: 0=0%, 1=25%, 2=50%, 3=75%, 4=100%, 5=125%, 6=150%, 7=175%,
    //  8=200%)
    {
        check(wvseg::kPresetCount == 9, "nine presets");
        check(wvseg::kPresetValues[0] == 0 && wvseg::kPresetValues[8] == 200,
              "presets span 0% to 200%");
        bool stepsOf25 = true;
        for (int i = 1; i < wvseg::kPresetCount; ++i) {
            stepsOf25 = stepsOf25 && wvseg::kPresetValues[i] - wvseg::kPresetValues[i - 1] == 25;
        }
        check(stepsOf25, "presets step by 25");

        QWidget parent;
        QButtonGroup group(&parent);
        group.setExclusive(true);
        QPushButton *buttons[wvseg::kPresetCount] = {};
        for (int i = 0; i < wvseg::kPresetCount; ++i) {
            buttons[i] = wvseg::createPresetButton(
                &parent, QStringLiteral("%1%").arg(wvseg::kPresetValues[i]), i == 0,
                i == wvseg::kPresetCount - 1);
            buttons[i]->setProperty("wvPresetValue", wvseg::kPresetValues[i]);
            group.addButton(buttons[i]);
            check(buttons[i]->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding,
                  "segments expand to fill the row");
            check(buttons[i]->property("wvSegFirst").toBool() == (i == 0)
                      && buttons[i]->property("wvSegLast").toBool()
                          == (i == wvseg::kPresetCount - 1),
                  "outer segments carry the edge properties");
        }

        wvseg::syncPresetGroup(&group, 100);
        check(group.checkedButton() == buttons[4], "100% selected by sync");
        check(buttons[4]->isChecked(), "100% button checked");

        // A value dragged between presets must leave the row with no
        // selection (an exclusive group cannot do this by itself).
        wvseg::syncPresetGroup(&group, 137);
        check(group.checkedButton() == nullptr, "no preset checked for a custom value");
        check(!buttons[4]->isChecked(), "previous selection cleared");
        check(group.exclusive(), "exclusivity restored after clearing");

        wvseg::syncPresetGroup(&group, 200);
        check(group.checkedButton() == buttons[8], "200% selected by sync");
        check(buttons[8]->isChecked(), "200% button checked");
        check(!buttons[0]->isChecked() && !buttons[4]->isChecked(), "only one segment checked");

        // Clicking a segment drives the slider; the valueChanged handler
        // re-syncs. Clicking the already-active one still re-asserts it.
        QSlider slider;
        slider.setRange(0, 200);
        slider.setValue(200);
        QObject::connect(&slider, &QSlider::valueChanged, [&group](int percent) {
            wvseg::syncPresetGroup(&group, percent);
        });
        for (QPushButton *button : buttons) {
            QObject::connect(button, &QPushButton::clicked, &slider, [&slider, button] {
                slider.setValue(button->property("wvPresetValue").toInt());
            });
        }

        buttons[2]->click();
        check(slider.value() == 50, "click moves the slider");
        check(group.checkedButton() == buttons[2], "click selects its segment");

        buttons[2]->click(); // unchanged value: no valueChanged emitted
        check(group.checkedButton() == buttons[2], "re-click keeps the segment checked");
        check(group.exclusive(), "exclusivity intact after re-click");

        group.setExclusive(false);
        buttons[5]->click();
        check(!buttons[2]->isChecked() && buttons[5]->isChecked(), "click with lifted exclusivity");
        group.setExclusive(true);
        wvseg::syncPresetGroup(&group, 50);
        check(group.checkedButton() == buttons[2], "sync recovers from a manual check");
    }

    // ---- 5. keyboard focus is visible on a checked segment ----
    {
        QWidget parent;
        auto *layout = new QVBoxLayout(&parent);
        QButtonGroup group(&parent);
        group.setExclusive(true);
        QPushButton *button = wvseg::createPresetButton(&parent, QStringLiteral("100%"), true, true);
        button->setProperty("wvPresetValue", 100);
        group.addButton(button);
        wvseg::syncPresetGroup(&group, 100);
        layout->addWidget(button);
        parent.show();
        app.processEvents();

        button->clearFocus();
        app.processEvents();
        const QImage unfocused = button->grab().toImage();

        button->setFocus(Qt::TabFocusReason);
        app.processEvents();
        const QImage focused = button->grab().toImage();

        check(button->hasFocus(), "segment accepts keyboard focus");
        check(focused.size() == unfocused.size(), "focus ring does not resize the segment");
        check(focused != unfocused, "focus ring is visible on a checked segment");
    }

    // ---- 6. null group is a no-op (startup ordering) ----
    wvseg::syncPresetGroup(nullptr, 100);
    check(true, "null group tolerated");

    fprintf(stderr, "preset_segment_test: total=%d failures=%d\n", s_total, s_failed);
    return s_failed ? 1 : 0;
}
