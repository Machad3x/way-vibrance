/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibranceeffect.h"

namespace KWin
{

KWIN_EFFECT_FACTORY_SUPPORTED(VibranceEffect, "metadata.json", return VibranceEffect::supported();)

} // namespace KWin

// KWIN_EFFECT_FACTORY_SUPPORTED expands into a Q_OBJECT plugin class; this
// pulls in its generated moc (plugin metadata + factory). AUTOMOC cannot see
// the macro by name, hence it is listed in AUTOMOC_MACRO_NAMES (effect/
// CMakeLists.txt) instead of silencing this include, which is NOT stale.
#include "main.moc"

