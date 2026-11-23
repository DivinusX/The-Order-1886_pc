//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  Frame generation composite (#47). Binding 0 is the interpolated frame band (RGBA16F,
//  sRGB-encoded values); the SDK leaves regions without motion data unwritten, and those
//  land as exact zeros. Binding 1 is the game's own finished frame. Wherever the generated
//  content is missing, the real frame shows through instead - UI overlays, prompts and any
//  other pixels the interpolator refused to produce - which replaces the old fixed-rectangle
//  re-blit that guessed at those regions and drew black boxes when the guess was wrong.

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D generated;
layout (binding = 1) uniform sampler2D real;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 encode(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

vec3 decode(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    const vec3 gen = texture(generated, uv).rgb;
    if (max(gen.r, max(gen.g, gen.b)) > 0.0) {
        color = vec4(encode(decode(gen)), 1.0);
        return;
    }
    const vec3 base = texture(real, uv).rgb;
    color = vec4(encode(decode(base)), 1.0);
}
