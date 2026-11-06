/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Versioned guarantees for the tray switch (tray/switch_p.h — the exact
    header the tray window compiles):

      1. it is still a QCheckBox: checkable, text preserved for screen
         readers and translations, one toggled() per activation;
      2. the whole row is a hit target (label and track), not just the
         painted track;
      3. Space (inherited) and Enter/Return toggle it;
      4. keyboard focus is visible on both states without changing size;
      5. checked/unchecked, enabled/disabled, light/dark and LTR/RTL all
         render differently (palette-derived, knob mirrored, no hard-coded
         theme);
      6. the size hint reserves room for the label and the track.

    Runs offscreen, no session bus needed. Exit code != 0 on failure.
*/

#include "switch_p.h"
#include "style_p.h"

#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

static int s_total = 0;
static int s_failed = 0;

static void check(bool ok, const char *what)
{
    ++s_total;
    if (!ok) {
        fprintf(stderr, "SWITCH-FAIL %s\n", what);
        ++s_failed;
    }
}

static QImage grab(QWidget *widget)
{
    QApplication::processEvents();
    return widget->grab().toImage();
}

static int colorDistance(const QColor &a, const QColor &b)
{
    return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
}

static QPalette darkPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#232629")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#1b1e20")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#eff0f1")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#eff0f1")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#4d4d4d")));
    palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#3a3d3f")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#3daee9")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    return palette;
}

static void sendMouseClick(QWidget *widget, const QPoint &pos)
{
    const QPointF local(pos);
    const QPointF global(widget->mapToGlobal(pos));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
    QApplication::processEvents();
}

static void sendKey(QWidget *widget, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
    QApplication::processEvents();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setStyleSheet(wvstyle::buildAppStyle(app.palette()));

    QWidget parent;
    auto *layout = new QVBoxLayout(&parent);
    auto *sw = new wvswitch::Switch(QStringLiteral("Only affect the primary monitor"), &parent);
    layout->addWidget(sw);
    parent.show();
    app.processEvents();

    // ---- 1. QCheckBox semantics and size contract ----
    check(qobject_cast<QCheckBox *>(sw) != nullptr, "keeps QCheckBox semantics");
    check(sw->isCheckable(), "is checkable");
    check(!sw->isChecked(), "starts unchecked");
    check(sw->text() == QStringLiteral("Only affect the primary monitor"),
          "label text preserved for accessibility");
    const QFontMetrics metrics(sw->fontMetrics());
    check(sw->sizeHint().width()
              >= metrics.horizontalAdvance(sw->text()) + 34 + 12,
          "size hint reserves label + track");
    check(sw->sizeHint().height() >= 18, "size hint gives a comfortable row");
    check(sw->minimumSizeHint().width() <= sw->sizeHint().width(),
          "long labels can elide in narrow cards");

    int toggles = 0;
    QObject::connect(sw, &QCheckBox::toggled, [&toggles](bool) { ++toggles; });

    // ---- 2. the whole row is clickable ----
    sendMouseClick(sw, QPoint(4, sw->height() / 2)); // over the label
    check(sw->isChecked(), "click on the label toggles");
    check(toggles == 1, "exactly one toggled() per click");

    sendMouseClick(sw, QPoint(sw->width() - 8, sw->height() / 2)); // over the track
    check(!sw->isChecked(), "click on the track toggles");
    check(toggles == 2, "one toggled() for the second click");

    // ---- 3. keyboard ----
    sendKey(sw, Qt::Key_Space);
    check(sw->isChecked(), "Space toggles");
    sendKey(sw, Qt::Key_Return);
    check(!sw->isChecked(), "Return toggles");
    sendKey(sw, Qt::Key_Enter);
    check(sw->isChecked(), "Enter toggles");

    // ---- 4. focus ring is visible and does not resize ----
    sw->clearFocus();
    const QImage unfocused = grab(sw);
    sw->setFocus(Qt::TabFocusReason);
    const QImage focused = grab(sw);
    check(sw->hasFocus(), "switch accepts keyboard focus");
    check(focused.size() == unfocused.size(), "focus ring does not resize");
    check(focused != unfocused, "focus ring is visible");

    // ---- 5. states, palettes and direction ----
    sw->setChecked(false);
    sw->clearFocus();
    const QImage offLight = grab(sw);
    sw->setFocus(Qt::TabFocusReason);
    const QImage offFocused = grab(sw);
    check(offFocused != offLight, "focus ring is visible on an unchecked switch");
    sw->setChecked(true);
    const QImage onLight = grab(sw);
    check(onLight != offLight, "checked and unchecked render differently");

    sw->setPalette(darkPalette());
    const QImage onDark = grab(sw);
    check(onDark != onLight, "palette changes are followed");

    sw->setEnabled(false);
    const QImage disabled = grab(sw);
    check(disabled != onDark, "disabled state renders differently");
    check(sw->text() == QStringLiteral("Only affect the primary monitor"),
          "disabled state keeps its accessible text");
    {
        // A disabled switch dims its colours, so Disabled WindowText and Mid
        // can coincide. The knob must still be distinguishable from the
        // track, otherwise the on/off position becomes invisible.
        const int midY = disabled.height() / 2;
        const QColor knob = disabled.pixelColor(disabled.width() - 11, midY);
        const QColor track = disabled.pixelColor(disabled.width() - 31, midY);
        check(colorDistance(knob, track) > 60,
              "disabled switch still shows the knob position");
    }
    sw->setEnabled(true);
    sw->setPalette(app.palette());

    // Knob mirroring: use an empty label so only the track geometry can
    // differ between LTR and RTL.
    auto *mirrored = new wvswitch::Switch(QString(), &parent);
    layout->addWidget(mirrored);
    mirrored->setChecked(true);
    app.processEvents();
    const QImage ltr = grab(mirrored);
    mirrored->setLayoutDirection(Qt::RightToLeft);
    const QImage rtl = grab(mirrored);
    check(ltr.size() == rtl.size(), "RTL keeps the same size");
    check(ltr != rtl, "knob mirrors in RTL");
    mirrored->setLayoutDirection(Qt::LeftToRight);
    mirrored->setChecked(false);
    const QImage emptyOff = grab(mirrored);
    check(emptyOff != ltr, "empty-label switch still shows state");

    fprintf(stderr, "switch_widget_test: total=%d failures=%d\n", s_total, s_failed);
    return s_failed ? 1 : 0;
}
