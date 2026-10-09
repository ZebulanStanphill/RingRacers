// SPDX-License-Identifier: GPL-2.0-only
#version 120
varying vec2 smoke_uv;
uniform sampler2D tex;
uniform vec4 poly_color;
uniform float marker_strength = 0.8;
void main()
{
    vec4 original = texture2D(tex, smoke_uv);
    vec2 cell = floor(smoke_uv * 8.0);
    float checker = mod(cell.x + cell.y, 2.0);
    vec3 marker = mix(vec3(1.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0), checker);
    gl_FragColor = vec4(mix(original.rgb, marker, marker_strength),
        original.a * poly_color.a);
}
