/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later

    Segmented preset control for the tray window and the per-app dialog.

    The presets (0/25/50/75/100/125/150/175/200%) behave as one exclusive
    group: exactly one button is checked while the slider sits on a preset
    value, and none is checked for any other value (a slider can be dragged
    to 137%). The set is shared by the tray window, the profile dialog and
    the tray menu so the three can never drift apart.

    Two details matter here:

      * QSS has no :first/:last pseudo-class, so the outer buttons carry
        "wvSegFirst"/"wvSegLast" properties that the application sheet
        (tray/style_p.h) uses to round the correct corners;
      * Qt refuses to uncheck the checked button of an exclusive group, so a
        non-preset value has to be cleared with exclusivity temporarily
        lifted — never by calling setChecked(false) directly.
*/

#pragma once

#include <QAbstractButton>
#include <QButtonGroup>
#include <QPushButton>
#include <QString>
#include <QWidget>

namespace wvseg {

/// The single preset set for every vibrance control in the app.
inline constexpr int kPresetValues[] = {0, 25, 50, 75, 100, 125, 150, 175, 200};
inline constexpr int kPresetCount =
    int(sizeof(kPresetValues) / sizeof(kPresetValues[0]));

/**
 * Creates one segment. The caller owns it (usually by adding it to a group
 * and a layout); the properties are read by the application stylesheet.
 * The button expands horizontally so a row of segments spans the card at
 * equal widths instead of hugging one side.
 */
inline QPushButton *createPresetButton(QWidget *parent, const QString &text, bool first, bool last)
{
    auto *button = new QPushButton(text, parent);
    button->setCheckable(true);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Exclusivity is provided by QButtonGroup; autoExclusive would fight it
    // when the group has to clear every button for a non-preset value.
    button->setAutoExclusive(false);
    button->setProperty("wvPreset", true);
    if (first) {
        button->setProperty("wvSegFirst", true);
    }
    if (last) {
        button->setProperty("wvSegLast", true);
    }
    return button;
}

/**
 * Marks @p button as the checked segment for @p value.
 *
 * Any other checked button is cleared first. Set the value on the button
 * with setProperty("wvPresetValue", value) when adding it to the group.
 */
inline void syncPresetGroup(QButtonGroup *group, int value)
{
    if (!group) {
        return;
    }

    // Lifting exclusivity is what allows "no preset selected": Qt otherwise
    // keeps the previously checked button checked.
    const bool wasExclusive = group->exclusive();
    group->setExclusive(false);

    bool matched = false;
    for (QAbstractButton *button : group->buttons()) {
        const bool isMatch = !matched && button->property("wvPresetValue").toInt() == value;
        button->setChecked(isMatch);
        if (isMatch) {
            matched = true;
        }
    }

    group->setExclusive(wasExclusive);
}

} // namespace wvseg
