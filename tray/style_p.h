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

#include <cmath>

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

inline qreal linearChannel(qreal value)
{
    return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

inline qreal relativeLuminance(const QColor &color)
{
    return 0.2126 * linearChannel(color.redF()) + 0.7152 * linearChannel(color.greenF())
        + 0.0722 * linearChannel(color.blueF());
}

inline qreal contrastRatio(const QColor &a, const QColor &b)
{
    const qreal la = relativeLuminance(a);
    const qreal lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

/// Semantic channel colour (red/green/blue) before theme adaptation.
inline QColor baseChannelColor(int index)
{
    static const QColor colors[3] = {
        QColor(0xe0, 0x55, 0x55),
        QColor(0x55, 0xc2, 0x5a),
        QColor(0x5b, 0x8d, 0xef),
    };
    return colors[qBound(0, index, 2)];
}

/**
 * Every colour the sheet needs, derived once from a palette.
 *
 * The channel colours keep their hue but are blended toward the scheme's
 * foreground until they reach 4.5:1 on the card surface: the stock red/green/
 * blue read fine on Breeze Dark but fail WCAG AA for small text on a light
 * card, and these colours are used for the R/G/B labels as well as the slider
 * handles.
 */
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
    QColor groove;
    QColor channel[3];
    QColor channelTint[3];
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
    colors.groove = blend(colors.card, colors.text, dark ? 0.14 : 0.12);

    for (int i = 0; i < 3; ++i) {
        const QColor base = baseChannelColor(i);
        QColor adapted = base;
        // 5% steps toward the foreground, capped at the foreground itself.
        for (int step = 1; step <= 20 && contrastRatio(adapted, colors.card) < 4.5; ++step) {
            adapted = blend(base, colors.text, step * 0.05);
        }
        colors.channel[i] = adapted;
        // Track fill: recognisably the channel, but soft enough not to read
        // as "100% = maximum colour" (100% is the neutral gain).
        colors.channelTint[i] = blend(adapted, colors.card, 0.55);
    }

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
        "QMainWindow, QDialog { background-color: %WINDOW%; }"

        // Cards: a subtle surface + floating section title instead of the
        // hard QGroupBox frame. The title lives in the top margin, so the
        // border never cuts through the text.
        "QGroupBox {"
        " background-color: %CARD%;"
        " border: 1px solid %BORDER%;"
        " border-radius: 8px;"
        " margin-top: 1.5em;"
        " padding: 12px;"
        "}"
        "QGroupBox::title {"
        " subcontrol-origin: margin;"
        " subcontrol-position: top left;"
        " left: 10px;"
        " padding: 0 4px;"
        " color: %MUTED%;"
        " font-weight: 600;"
        "}"

        "QListWidget {"
        " background-color: %BASE%;"
        " border: 1px solid %BORDER%;"
        " border-radius: 6px;"
        " padding: 4px;"
        "}"
        "QListWidget::item { padding: 6px 8px; border-radius: 4px; }"
        "QListWidget::item:selected { background-color: %HIGHLIGHT%; color: %HIGHLIGHTED_TEXT%; }"

        "QLineEdit, QComboBox {"
        " background-color: %BASE%;"
        " border: 1px solid %BORDER%;"
        " border-radius: 6px;"
        " padding: 4px 8px;"
        " min-height: 22px;"
        " selection-background-color: %HIGHLIGHT%;"
        " selection-color: %HIGHLIGHTED_TEXT%;"
        "}"
        "QLineEdit:focus, QComboBox:focus { border: 1px solid %HIGHLIGHT%; }"
        "QLineEdit:disabled, QComboBox:disabled { color: %MUTED%; }"

        "QCheckBox { spacing: 8px; }"
        "QLabel[wvHint=\"true\"] { color: %MUTED%; }"

        // Channel labels: fixed semantic colours, palette-adapted above.
        "QLabel[wvChannel=\"R\"] { color: %CH_R%; font-weight: bold; }"
        "QLabel[wvChannel=\"G\"] { color: %CH_G%; font-weight: bold; }"
        "QLabel[wvChannel=\"B\"] { color: %CH_B%; font-weight: bold; }"

        // Sliders. Styling sub-controls makes QStyleSheetStyle own the whole
        // slider, so every state we care about is spelled out here.
        "QSlider { min-height: 22px; }"
        "QSlider::groove:horizontal {"
        " height: 6px;"
        " border-radius: 3px;"
        " background-color: %GROOVE%;"
        "}"
        "QSlider::sub-page:horizontal { border-radius: 3px; background-color: %HIGHLIGHT%; }"
        "QSlider::add-page:horizontal { border-radius: 3px; background-color: %GROOVE%; }"
        "QSlider::handle:horizontal {"
        " width: 16px;"
        " height: 16px;"
        " margin: -5px 0px;"
        " border-radius: 8px;"
        " border: 2px solid %CARD%;"
        " background-color: %HIGHLIGHT%;"
        "}"
        "QSlider[wvChannel=\"R\"]::sub-page:horizontal { background-color: %TINT_R%; }"
        "QSlider[wvChannel=\"R\"]::handle:horizontal { background-color: %CH_R%; }"
        "QSlider[wvChannel=\"G\"]::sub-page:horizontal { background-color: %TINT_G%; }"
        "QSlider[wvChannel=\"G\"]::handle:horizontal { background-color: %CH_G%; }"
        "QSlider[wvChannel=\"B\"]::sub-page:horizontal { background-color: %TINT_B%; }"
        "QSlider[wvChannel=\"B\"]::handle:horizontal { background-color: %CH_B%; }"
        // The channel rules above outrank the generic pseudo-state rules on
        // specificity, so the disabled look is repeated per channel.
        "QSlider[wvChannel=\"R\"]::handle:horizontal:disabled { background-color: %MUTED%; }"
        "QSlider[wvChannel=\"R\"]::sub-page:horizontal:disabled { background-color: %MUTED%; }"
        "QSlider[wvChannel=\"G\"]::handle:horizontal:disabled { background-color: %MUTED%; }"
        "QSlider[wvChannel=\"G\"]::sub-page:horizontal:disabled { background-color: %MUTED%; }"
        "QSlider[wvChannel=\"B\"]::handle:horizontal:disabled { background-color: %MUTED%; }"
        "QSlider[wvChannel=\"B\"]::sub-page:horizontal:disabled { background-color: %MUTED%; }"
        "QSlider::handle:horizontal:hover { border-color: %HIGHLIGHT%; }"
        "QSlider::handle:horizontal:focus { border-color: %HIGHLIGHT%; }"
        "QSlider::handle:horizontal:disabled { background-color: %MUTED%; }"
        "QSlider::sub-page:horizontal:disabled { background-color: %MUTED%; }"

        // Preset segmented control (tray/segmented_p.h). Buttons are
        // property-marked so the Apply/OK/Cancel buttons stay native.
        "QPushButton[wvPreset=\"true\"] {"
        " background-color: %BASE%;"
        " border: 1px solid %BORDER%;"
        " border-left: none;"
        " border-radius: 0px;"
        " padding: 5px 12px;"
        " min-height: 22px;"
        "}"
        "QPushButton[wvPreset=\"true\"][wvSegFirst=\"true\"] {"
        " border-left: 1px solid %BORDER%;"
        " border-top-left-radius: 6px;"
        " border-bottom-left-radius: 6px;"
        "}"
        "QPushButton[wvPreset=\"true\"][wvSegLast=\"true\"] {"
        " border-top-right-radius: 6px;"
        " border-bottom-right-radius: 6px;"
        "}"
        "QPushButton[wvPreset=\"true\"]:hover { background-color: %MIDLIGHT%; }"
        "QPushButton[wvPreset=\"true\"]:checked {"
        " background-color: %HIGHLIGHT%;"
        " color: %HIGHLIGHTED_TEXT%;"
        " border-color: %HIGHLIGHT%;"
        "}"
        "QPushButton[wvPreset=\"true\"]:checked:hover {"
        " background-color: %HIGHLIGHT%;"
        " color: %HIGHLIGHTED_TEXT%;"
        "}"
        "QPushButton[wvPreset=\"true\"]:pressed {"
        " background-color: %HIGHLIGHT%;"
        " color: %HIGHLIGHTED_TEXT%;"
        "}"
        "QPushButton[wvPreset=\"true\"]:focus { border-color: %HIGHLIGHT%; }"

        // Compact clear action for the shortcut field.
        "QToolButton[wvClear=\"true\"] { border: none; border-radius: 4px; padding: 3px; }"
        "QToolButton[wvClear=\"true\"]:hover { background-color: %MIDLIGHT%; }"
        "QToolButton[wvClear=\"true\"]:pressed { background-color: %MID%; }"
        "QToolButton[wvClear=\"true\"]:focus { border: 1px solid %HIGHLIGHT%; }"

        // Circular per-slider reset button (see tray/resetbutton_p.h). The
        // marker property is what keeps other QToolButtons native.
        "QToolButton[wvReset=\"true\"] {"
        " border: 1px solid %MID%;"
        " border-radius: 9px;"
        " padding: 0px;"
        " font-size: 11px;"
        " color: %BUTTON_TEXT%;"
        "}"
        "QToolButton[wvReset=\"true\"]:hover { background-color: %MIDLIGHT%; }"
        "QToolButton[wvReset=\"true\"]:pressed { background-color: %MID%; }"
        "QToolButton[wvReset=\"true\"]:focus { border: 1px solid %HIGHLIGHT%; }");

    style.replace(QStringLiteral("%WINDOW%"), c.window.name());
    style.replace(QStringLiteral("%CARD%"), c.card.name());
    style.replace(QStringLiteral("%BORDER%"), c.border.name());
    style.replace(QStringLiteral("%MUTED%"), c.muted.name());
    style.replace(QStringLiteral("%BASE%"), c.base.name());
    style.replace(QStringLiteral("%HIGHLIGHTED_TEXT%"), c.highlightedText.name());
    style.replace(QStringLiteral("%HIGHLIGHT%"), c.highlight.name());
    style.replace(QStringLiteral("%MIDLIGHT%"), c.midlight.name());
    style.replace(QStringLiteral("%MID%"), c.mid.name());
    style.replace(QStringLiteral("%BUTTON_TEXT%"), c.buttonText.name());
    style.replace(QStringLiteral("%GROOVE%"), c.groove.name());
    style.replace(QStringLiteral("%CH_R%"), c.channel[0].name());
    style.replace(QStringLiteral("%CH_G%"), c.channel[1].name());
    style.replace(QStringLiteral("%CH_B%"), c.channel[2].name());
    style.replace(QStringLiteral("%TINT_R%"), c.channelTint[0].name());
    style.replace(QStringLiteral("%TINT_G%"), c.channelTint[1].name());
    style.replace(QStringLiteral("%TINT_B%"), c.channelTint[2].name());
    return style;
}

} // namespace wvstyle
