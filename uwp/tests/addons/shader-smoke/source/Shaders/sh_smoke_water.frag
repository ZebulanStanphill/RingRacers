// SPDX-License-Identifier: GPL-2.0-only
#version 120
uniform sampler2D tex;
uniform vec4 poly_color;
uniform float leveltime;
void main()
{
    vec2 uv = gl_TexCoord[0].st;
    vec4 original = texture2D(tex, uv);
    float wave = 0.5 + 0.5 * sin(uv.x * 12.0 + uv.y * 7.0 + leveltime * 2.0);
    vec3 marker = mix(vec3(0.0, 1.0, 1.0), vec3(1.0, 0.4, 0.0), wave);
    gl_FragColor = vec4(mix(original.rgb, marker, 0.85),
        original.a * poly_color.a);
}
