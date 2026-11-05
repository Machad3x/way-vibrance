#version 140
// SPDX-FileCopyrightText: 2026 machadex
// SPDX-License-Identifier: GPL-2.0-or-later

// Per-channel RGB gain pass for the Way Vibrance effect.
// Windows reach this shader already color-graded by KWin's built-in linear
// saturation uniform (the "vibrance"), which OffscreenEffect applies while
// grabbing; here we only scale the channels, also in linear space, so both
// adjustments behave consistently and match the scene colorspace.

#include "colormanagement.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;
uniform vec3 rgbGain;

in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 tex = texture2D(sampler, texcoord0);
    tex = sourceEncodingToNitsInDestinationColorspace(tex);
    tex.rgb *= rgbGain;
    fragColor = nitsToDestinationEncoding(tex * modulation);
}
