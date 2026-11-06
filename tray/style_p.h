/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Application stylesheet for the tray window and the per-app profile dialog.

    The sheet is generated from the live QPalette instead of hard-coded
    colours: KDE ships light and dark schemes (and Plasma can switch them at
    runtime), so a fixed "Breeze Dark" palette would break half of them.

    Two hard rules, both covered by tests/theme_style_test.cpp:

      * colours only — no url(), no image(), no file paths, so a stylesheet
        can never become a way to load external resources;
      * no per-widget setStyleSheet() calls for the theme, so a palette
        change re-polishes every widget from one place.
*/

#pragma once

#include <QColor>
#include <QPalette>
#include <QString>
#include <QStringLiteral>

namespace wvstyle {

// Linear blend between two opaque colours (amount 0 => from, 1 => to).
// Blending in C++ keeps the generated QSS free of rgba()/alpha syntax whose
// parsing differs between Qt versions and platform styles.
inline QColor blend(const QColor &from, const QColor &to, qreal amount)
{
    const qreal t = qBound<qreal>(0.0, amount, 1.0);
    return QColor::fromRgbF(from.redF() * (1.0 - t) + to.redF() * t,
                            from.greenF() * (1.0 - t) + to.greenF() * t,
                            from.blueF() * (1.0 - t) + to.blueF() * t);
}

/// Every colour the sheet needs, derived once from a palette.
struct ThemeColors
{
    QColor window;
    QColor base;
    QColor text;
    QColor buttonText;
    QColor highlight;
    QColor highlightedText;
    QColor mid;
    QColor midlight;
    QColor card;
    QColor border;
    QColor muted;
};

inline ThemeColors themeColors(const QPalette &palette)
{
    ThemeColors colors;
    colors.window = palette.color(QPalette::Window);
    colors.base = palette.color(QPalette::Base);
    colors.text = palette.color(QPalette::WindowText);
    colors.buttonText = palette.color(QPalette::ButtonText);
    colors.highlight = palette.color(QPalette::Highlight);
    colors.highlightedText = palette.color(QPalette::HighlightedText);
    colors.mid = palette.color(QPalette::Mid);
    colors.midlight = palette.color(QPalette::Midlight);

    // Card surfaces are a blend of Window and Base: darker than the window
    // in a dark scheme, lighter in a light one, so one formula serves both.
    const bool dark = colors.window.lightness() < 128;
    colors.card = blend(colors.window, colors.base, 0.55);
    colors.border = blend(colors.window, colors.text, dark ? 0.16 : 0.14);
    // Secondary text needs a little more foreground in a light scheme to
    // stay above 4.5:1 on the card surface (checked by the theme test).
    colors.muted = blend(colors.window, colors.text, dark ? 0.55 : 0.65);

    return colors;
}

/**
 * Builds the whole application stylesheet from @p palette.
 *
 * Borders and secondary text are blends of the foreground colour into the
 * window, which keeps their contrast tied to the scheme instead of a fixed
 * grey. No colour is written by hand here.
 */
inline QString buildAppStyle(const QPalette &palette)
{
    const ThemeColors c = themeColors(palette);

    QString style = QStringLiteral(
        "QMainWindow, QDialog { background-color: %1; }"

        // Cards: a subtle surface + floating section title instead of the
        // hard QGroupBox frame. The title lives in the top margin, so the
        // border never cuts through the text.
        "QGroupBox {"
        " background-color: %2;"
        " border: 1px solid %3;"
        " border-radius: 8px;"
        " margin-top: 1.5em;"
        " padding: 12px;"
        "}"
        "QGroupBox::title {"
        " subcontrol-origin: margin;"
        " subcontrol-position: top left;"
        " left: 10px;"
        " padding: 0 4px;"
        " color: %4;"
        " font-weight: 600;"
        "}"

        "QListWidget {"
        " background-color: %5;"
        " border: 1px solid %3;"
        " border-radius: 6px;"
        " padding: 4px;"
        "}"
        "QListWidget::item { padding: 6px 8px; border-radius: 4px; }"
        "QListWidget::item:selected { background-color: %6; color: %7; }"

        "QLineEdit, QComboBox {"
        " background-color: %5;"
        " border: 1px solid %3;"
        " border-radius: 6px;"
        " padding: 4px 8px;"
        " min-height: 22px;"
        " selection-background-color: %6;"
        " selection-color: %7;"
        "}"
        "QLineEdit:focus, QComboBox:focus { border: 1px solid %6; }"
        "QLineEdit:disabled, QComboBox:disabled { color: %4; }"

        "QCheckBox { spacing: 8px; }"
        "QLabel[wvHint=\"true\"] { color: %4; }"

        // Circular per-slider reset button (see tray/resetbutton_p.h). The
        // marker property is what keeps other QToolButtons native.
        "QToolButton[wvReset=\"true\"] {"
        " border: 1px solid %8;"
        " border-radius: 9px;"
        " padding: 0px;"
        " font-size: 11px;"
        " color: %9;"
        "}"
        "QToolButton[wvReset=\"true\"]:hover { background-color: %10; }"
        "QToolButton[wvReset=\"true\"]:pressed { background-color: %8; }"
        "QToolButton[wvReset=\"true\"]:focus { border: 1px solid %6; }");

    return style.arg(c.window.name())
        .arg(c.card.name())
        .arg(c.border.name())
        .arg(c.muted.name())
        .arg(c.base.name())
        .arg(c.highlight.name())
        .arg(c.highlightedText.name())
        .arg(c.mid.name())
        .arg(c.buttonText.name())
        .arg(c.midlight.name());
}

} // namespace wvstyle
