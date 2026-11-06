/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Versioned guarantees for the tray/profile stylesheet (tray/style_p.h):

      1. it is derived from the QPalette, so a light scheme and a dark scheme
         produce different sheets and a custom highlight colour really ends
         up in the sheet (no hard-coded Breeze Dark colours);
      2. secondary text and normal text keep >= 4.5:1 contrast on the card
         surface in both schemes (WCAG AA for small text), and the slider
         groove keeps >= 3:1 (WCAG 1.4.11) so the track stays visible at
         low values, with the handle hover/focus border in the scheme
         foreground instead of the handle's own fill colour;
      3. it contains no external resource loading — no url(), image(),
         @import or file:// (a stylesheet must never read user-writable
         paths), and it is brace-balanced;
      4. blend() clamps out-of-range amounts instead of extrapolating;
      5. the R/G/B channel colours keep their semantic hue, are pairwise
         distinct and reach >= 4.5:1 on the card in both schemes, and the
         sheet exposes the channel/preset/clear selectors without leaving
         any %TOKEN% unreplaced.

    Runs headless (QtCore/Gui only). Exit code != 0 on failure.
*/

#include "style_p.h"

#include <QColor>
#include <QPalette>

#include <cmath>
#include <cstdio>

static int s_total = 0;
static int s_failed = 0;

static void check(bool ok, const char *what)
{
    ++s_total;
    if (!ok) {
        fprintf(stderr, "THEME-STYLE-FAIL %s\n", what);
        ++s_failed;
    }
}

static qreal linearChannel(qreal value)
{
    return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

static qreal luminance(const QColor &color)
{
    return 0.2126 * linearChannel(color.redF()) + 0.7152 * linearChannel(color.greenF())
        + 0.0722 * linearChannel(color.blueF());
}

static qreal contrastRatio(const QColor &a, const QColor &b)
{
    const qreal la = luminance(a);
    const qreal lb = luminance(b);
    const qreal lighter = qMax(la, lb);
    const qreal darker = qMin(la, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

static QPalette makePalette(bool dark, const QColor &highlight = QColor(QStringLiteral("#3daee9")))
{
    QPalette palette;
    if (dark) {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#232629")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#1b1e20")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#eff0f1")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#eff0f1")));
        palette.setColor(QPalette::Mid, QColor(QStringLiteral("#4d4d4d")));
        palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#3a3d3f")));
    } else {
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#eff0f1")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#232629")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#232629")));
        palette.setColor(QPalette::Mid, QColor(QStringLiteral("#bdc3c7")));
        palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#d5d8da")));
    }
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    return palette;
}

static void checkSheet(const char *label, const QPalette &palette)
{
    const wvstyle::ThemeColors colors = wvstyle::themeColors(palette);
    const QString sheet = wvstyle::buildAppStyle(palette);

    check(!sheet.isEmpty(), label);
    check(sheet.contains(QStringLiteral("QGroupBox")), "cards styled");
    check(sheet.contains(QStringLiteral("border-radius: 8px")), "card radius");
    check(sheet.contains(QStringLiteral("wvReset")), "reset button hook");
    check(sheet.contains(QStringLiteral("wvChannel")), "channel selector");
    check(sheet.contains(QStringLiteral("wvPreset")), "preset selector");
    check(sheet.contains(QStringLiteral("wvSegFirst")), "first segment hook");
    check(sheet.contains(QStringLiteral("wvSegLast")), "last segment hook");
    check(sheet.contains(QStringLiteral("wvClear")), "clear action hook");
    check(sheet.contains(colors.window.name()), "window colour from palette");
    check(sheet.contains(colors.card.name()), "card colour from palette");
    check(sheet.contains(colors.highlight.name()), "highlight colour from palette");

    // 2. contrast (AA) of normal and secondary text on the card surface.
    check(contrastRatio(colors.text, colors.card) >= 4.5, "text contrast >= 4.5 on card");
    check(contrastRatio(colors.muted, colors.card) >= 4.5, "muted contrast >= 4.5 on card");

    // The slider groove is the track itself: it must stay visible at low
    // values where the filled sub-page is empty (WCAG 1.4.11, 3:1).
    check(contrastRatio(colors.groove, colors.card) >= 3.0, "groove contrast >= 3 on card");
    check(colors.groove != colors.card, "groove differs from the card");
    check(sheet.contains(colors.groove.name()), "groove colour from palette adaptation");

    // The generic handle is filled with Highlight: a Highlight hover/focus
    // border would be invisible on it, so both use the scheme foreground.
    check(sheet.contains(QStringLiteral("QSlider::handle:horizontal:focus { border-color: ")
                         + colors.buttonText.name()),
          "slider handle focus uses the foreground, not the fill");
    check(sheet.contains(QStringLiteral("QSlider::handle:horizontal:hover { border-color: ")
                         + colors.buttonText.name()),
          "slider handle hover uses the foreground, not the fill");

    // 5. channel colours stay semantic, distinct and legible on the card.
    for (int i = 0; i < 3; ++i) {
        const QColor &channel = colors.channel[i];
        check(contrastRatio(channel, colors.card) >= 4.5, "channel contrast >= 4.5 on card");
        check(sheet.contains(channel.name()), "channel colour from palette adaptation");
        check(sheet.contains(colors.channelTint[i].name()), "channel tint in the sheet");
        check(colors.channelTint[i] != colors.card, "tint differs from the card");
        check(colors.channelTint[i] != channel, "tint differs from the handle");
    }
    // Red keeps red dominant, green green, blue blue (blending toward the
    // foreground preserves the order for these base colours).
    check(colors.channel[0].redF() > colors.channel[0].greenF()
              && colors.channel[0].redF() > colors.channel[0].blueF(),
          "red channel stays red");
    check(colors.channel[1].greenF() > colors.channel[1].redF()
              && colors.channel[1].greenF() > colors.channel[1].blueF(),
          "green channel stays green");
    check(colors.channel[2].blueF() > colors.channel[2].redF()
              && colors.channel[2].blueF() > colors.channel[2].greenF(),
          "blue channel stays blue");

    // Pairwise distinctness: a delta of a few units would make the rows
    // indistinguishable on screen.
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            const int delta = qAbs(colors.channel[i].red() - colors.channel[j].red())
                + qAbs(colors.channel[i].green() - colors.channel[j].green())
                + qAbs(colors.channel[i].blue() - colors.channel[j].blue());
            check(delta >= 30, "channels are visually distinct");
        }
    }

    // Every placeholder of the generator was substituted.
    static const char *const tokens[] = {
        "%WINDOW%", "%CARD%", "%BORDER%", "%MUTED%", "%BASE%",
        "%HIGHLIGHT%", "%HIGHLIGHTED_TEXT%", "%MID%",
        "%MIDLIGHT%", "%BUTTON_TEXT%", "%GROOVE%",
        "%CH_R%", "%CH_G%", "%CH_B%",
        "%TINT_R%", "%TINT_G%", "%TINT_B%",
    };
    for (const char *token : tokens) {
        check(!sheet.contains(QLatin1String(token)), "no unreplaced QSS token");
    }

    // 3. no external resources, balanced braces.
    check(!sheet.contains(QStringLiteral("url(")), "no url()");
    check(!sheet.contains(QStringLiteral("image(")), "no image()");
    check(!sheet.contains(QStringLiteral("@import")), "no @import");
    check(!sheet.contains(QStringLiteral("file://")), "no file://");
    check(sheet.count(QLatin1Char('{')) == sheet.count(QLatin1Char('}')), "balanced braces");

    // Deterministic output for the same palette.
    check(sheet == wvstyle::buildAppStyle(palette), "deterministic for one palette");
}

int main()
{
    const QPalette dark = makePalette(true);
    const QPalette light = makePalette(false);

    checkSheet("dark sheet built", dark);
    checkSheet("light sheet built", light);

    // 1. the sheet follows the palette, not a baked-in theme.
    check(wvstyle::buildAppStyle(dark) != wvstyle::buildAppStyle(light),
          "light and dark sheets differ");

    const QPalette red = makePalette(true, QColor(QStringLiteral("#ff0000")));
    const QString redSheet = wvstyle::buildAppStyle(red);
    check(redSheet.contains(QStringLiteral("#ff0000")), "custom highlight reaches the sheet");
    check(!redSheet.contains(QStringLiteral("#3daee9")), "no hard-coded Breeze blue");

    // Card direction: darker than the window in a dark scheme, lighter in a
    // light one (so the same formula serves both).
    check(wvstyle::themeColors(dark).card.lightness()
              < wvstyle::themeColors(dark).window.lightness(),
          "dark card sits below the window");
    check(wvstyle::themeColors(light).card.lightness()
              > wvstyle::themeColors(light).window.lightness(),
          "light card sits above the window");

    // 4. blend() clamps.
    const QColor from(QStringLiteral("#102030"));
    const QColor to(QStringLiteral("#a0b0c0"));
    check(wvstyle::blend(from, to, -5.0) == from, "blend clamps low");
    check(wvstyle::blend(from, to, 5.0) == to, "blend clamps high");
    check(wvstyle::blend(from, to, 0.0) == from, "blend 0 returns from");
    check(wvstyle::blend(from, to, 1.0) == to, "blend 1 returns to");

    fprintf(stderr, "theme_style_test: total=%d failures=%d\n", s_total, s_failed);
    return s_failed ? 1 : 0;
}
