/*
    SPDX-FileCopyrightText: 2026 machadex

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vibranceeffect.h"

namespace KWin
{

KWIN_EFFECT_FACTORY_SUPPORTED(VibranceEffect, "metadata.json", return VibranceEffect::supported();)

} // namespace KWin

#include "main.moc"
