// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
// Stand-ins for the game's screen effect shaders, which are game data and not part of this repository. They use the same
// RHI macros and declarations as the game's rhi_glsl_fragment_crt, but are written for the tests: a small compatibility
// library (the Vulkan-style macros only) and a fragment shader whose output is easy to predict.
#ifndef RR_TEST_SCREEN_EFFECT_FIXTURES_H
#define RR_TEST_SCREEN_EFFECT_FIXTURES_H

static const char kStandInCompat[] = R"(// Stand-in for rhi_glsl_lib_versioncompat
#if !defined(GL_ES) && __VERSION__ >= 450
#define BACKEND_VULKAN
#endif
#if defined(BACKEND_VULKAN)
#define FRAGMENT_VARYING_IN(loc) layout(location = loc) in
#define UNIFORM(s,b) layout(set = s, binding = b) uniform
#define DECLARE_FRAGMENT_COLOR_OUTPUT layout(location = 0) out vec4 o_color;
#define OUTPUT_COLOR o_color
#define TEXTURE2D texture
#endif
)";

// For a frame of width W and height H pixels, whose pixel (r, g, b) the output is (r, g * W / 128, 0.5 * H / 64): the
// frame at v_texcoord0 (the loop makes it sample inside a loop, as the game's shaders do), the frame's size, and the
// dot pattern's first texel (red), read at a coordinate that is outside it so that it only wraps to it. A 64x64 frame
// gives (r, g / 2, 128).
static const char kStandInEffect[] = R"(// Stand-in for rhi_glsl_fragment_crt
FRAGMENT_VARYING_IN(0) vec2 v_texcoord0;
FRAGMENT_VARYING_IN(1) vec4 v_color;
DECLARE_FRAGMENT_COLOR_OUTPUT

// Group 1
UNIFORM(1,0) mat4 u_projection;

// Group 2
UNIFORM(2,0) mat4 u_modelview;
UNIFORM(2,1) mat3 u_texcoord0_transform;
UNIFORM(2,2) vec2 u_sampler0_size;

UNIFORM(0,0) sampler2D s_sampler0;
UNIFORM(0,1) sampler2D s_sampler1; // dot pattern 12x4

void main()
{
	vec3 frame = vec3(0.0);
	for (float i = 0.0; i < 1.0; i += 0.5)
	{
		frame += TEXTURE2D(s_sampler0, v_texcoord0 + vec2(0.0, i * 0.0)).rgb * 0.5;
	}
	vec3 dots = TEXTURE2D(s_sampler1, vec2(1.0 + 1.0 / 24.0, 1.0 / 8.0)).rgb;
	OUTPUT_COLOR = vec4(frame.r, frame.g * (u_sampler0_size.x / 128.0), 0.5 * dots.r * (u_sampler0_size.y / 64.0), 1.0);
}
)";

// The same without the frame's size: only the program, not the uniform, is optional
static const char kStandInNoSize[] = R"(FRAGMENT_VARYING_IN(0) vec2 v_texcoord0;
DECLARE_FRAGMENT_COLOR_OUTPUT
UNIFORM(0,0) sampler2D s_sampler0;
void main() { OUTPUT_COLOR = vec4(TEXTURE2D(s_sampler0, v_texcoord0).rgb, 1.0); }
)";
#endif
