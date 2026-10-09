// SPDX-License-Identifier: GPL-2.0-only
#version 120
varying vec2 smoke_uv;
void main()
{
    smoke_uv = gl_MultiTexCoord0.st;
    gl_TexCoord[0] = gl_MultiTexCoord0;
    gl_FrontColor = gl_Color;
    gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex;
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
}
