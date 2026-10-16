// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file shaders.hlsl
/// \brief [UWP]: Legacy GL's shaders for its Direct3D 12 driver (r_d3d12.cpp)
///
/// The GLSL shaders of r_opengl.c, ported to HLSL, plus the parts of OpenGL's
/// fixed-function pipeline Legacy GL uses when its shaders are off. Each entry
/// point is compiled by fxc at build time (see ../CMakeLists.txt).

// [UWP]: Shared 25-DWORD root signature; b4 reserves the custom view constants.
#ifdef RR_D3D12
#define RR_ROOT_SIGNATURE "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | DENY_HULL_SHADER_ROOT_ACCESS | DENY_DOMAIN_SHADER_ROOT_ACCESS | DENY_GEOMETRY_SHADER_ROOT_ACCESS), " \
	"CBV(b0), " \
	"DescriptorTable(SRV(t0), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(Sampler(s0), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t1), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(Sampler(s1), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t3), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t2), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t4), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t5), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t6), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t7), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t8), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t9), visibility=SHADER_VISIBILITY_PIXEL), " \
	"DescriptorTable(SRV(t10), visibility=SHADER_VISIBILITY_PIXEL), " \
	"CBV(b1, visibility=SHADER_VISIBILITY_PIXEL), " \
	"CBV(b2, visibility=SHADER_VISIBILITY_PIXEL), " \
	"CBV(b3, visibility=SHADER_VISIBILITY_PIXEL), " \
	"CBV(b4), " \
	"CBV(b5, visibility=SHADER_VISIBILITY_PIXEL)"
#define RR_RS [RootSignature(RR_ROOT_SIGNATURE)]
#else
#define RR_RS
#endif

// Must match Constants in r_d3d12.cpp
cbuffer DrawConstants : register(b0)
{
	float4x4 u_projection; // column-major, as OpenGL stores them
	float4x4 u_modelview;
	float4 u_poly_color;
	float4 u_tint_color;
	float4 u_fade_color;
	float4 u_color; // glColor: the vertex color of arrays that have none
	float4 u_light_dir; // xyz: the shaders' light_dir uniform
	float4 u_ff_light; // xyz: GL_LIGHT0's eye-space direction for fixed-function model lighting; w: 1 if lit
	float u_lighting;
	float u_fade_start;
	float u_fade_end;
	float u_light_contrast;
	float u_light_backlight;
	float u_leveltime;
	float u_alpha_ref;
	int u_alpha_func; // 0: GL_ALWAYS, 1: GL_GREATER, 2: GL_NOTEQUAL
	int u_tex_mode; // fixed-function texturing: 0: none (the vertex color), 1: GL_MODULATE, 2: GL_REPLACE
	int u_palette; // palette rendering, with a light table
	int2 u_pad;
#ifdef _UWP
	float4 u_clip_plane; // camera space, evaluated after the full modelview
	float u_fullscreen_depth;
	float3 u_portal_pad;
	float4 u_postimg_rect; // min/max UV in the square captured texture
	float4 u_postimg_info; // tic time, postimg_t, full screen UV extent
#endif
};

Texture2D t_tex : register(t0);
Texture2D t_brightmap : register(t1); // also the fade mask of screen wipes
// Palette rendering, from SRB2
Texture3D<float> t_palette_lookup : register(t2); // RGB -> palette index / 255, 64 on each side
Texture2D t_lighttable : register(t3); // a colormap's 32 light levels of 256 colors, then the base colormap's level 0
Texture2D t_screen_palette : register(t4); // 256 colors: the palette the finished frame is shown in
Texture2D<float> t_wipe_mask : register(t5); // raw lump bytes / 255, leaving the palette slots intact
Texture2D<float> t_fade_map : register(t6); // 256 columns of palette indices / 255 per strength row
Texture2D t_master_palette : register(t7); // uncorrected palette used to make palette-rendering textures
SamplerState s_tex : register(s0);
SamplerState s_brightmap : register(s1);

// Must match WipeConstants in d3d12_screens.inc.
cbuffer WipeConstants : register(b1)
{
	float2 u_wipe_screen_size;
	float2 u_wipe_screen_fix;
	int u_wipe_mode;
	int u_wipe_wiggle;
	int u_wipe_reverse;
	int u_wipe_pad;
};

// Must match FadeMapConstants in d3d12_screens.inc. Keep the current draw's b0/ring range intact.
cbuffer FadeMapConstants : register(b2)
{
	int u_fade_map_row;
	int3 u_fade_map_pad;
};

#ifdef _UWP
Texture2D t_fog_scene : register(t8);
cbuffer FogConstants : register(b3)
{
	int u_fog_plane;
	int u_fog_lightnum;
	float u_fog_view_ratio;
	int u_fog_pad;
};
#endif

struct VSOut
{
	float4 pos : SV_Position;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	// gl_FragCoord.z / gl_FragCoord.w: window depth times clip w, which is (z + w) / 2 of the clip position,
	// interpolated with perspective
	float fragz : TEXCOORD2;
#ifdef _UWP
	float clipDistance : SV_ClipDistance0;
#endif
};

float4 Transform(float3 pos)
{
	return mul(u_projection, mul(u_modelview, float4(pos, 1.0)));
}

// OpenGL clips z to [-w, w] and Direct3D to [0, w]
VSOut Finish(float4 clip, float4 color, float2 uv)
{
	VSOut o;
	float z = 0.5 * (clip.z + clip.w);
	o.pos = float4(clip.x, clip.y, z, clip.w);
	o.color = color;
	o.uv = uv;
	o.uv1 = float2(0.0, 0.0);
	o.fragz = z;
#ifdef _UWP
	o.clipDistance = 1.0;
#endif
	return o;
}

#ifdef _UWP
VSOut FinishWorld(float3 pos, float4 clip, float4 color, float2 uv)
{
	VSOut o = Finish(clip, color, uv);
	o.clipDistance = dot(mul(u_modelview, float4(pos, 1.0)), u_clip_plane);
	return o;
}
#else
#define FinishWorld(pos, clip, color, uv) Finish(clip, color, uv)
#endif

// ================
//  Vertex shaders
// ================

// FOutVector, with the current glColor
RR_RS
VSOut VSPoly(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
	return FinishWorld(pos, Transform(pos), u_color, uv);
}

// GLSL_SPRITECLIP_HACK_VERTEX_SHADER
RR_RS
VSOut VSPolyClip(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
	float4 clip = Transform(pos);
	clip.z -= 80.0 / clip.z;
	return FinishWorld(pos, clip, u_color, uv);
}

// gl_skyvertex_t, with its own colors
RR_RS
VSOut VSSky(float3 pos : POSITION, float2 uv : TEXCOORD0, float4 color : COLOR0)
{
	return Finish(Transform(pos), color, uv);
}

// Models without shaders, lit like OpenGL's fixed function does with Legacy GL's settings when model lighting is on:
// a scene ambient of 1 and a directional GL_LIGHT0, with the surface color as the ambient and diffuse material
RR_RS
VSOut VSModel(float3 pos : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0)
{
	float4 color = u_color;

	if (u_ff_light.w > 0.0)
	{
		float3 n = normalize(mul((float3x3)u_modelview, normal));
		float diffuse = max(dot(n, u_ff_light.xyz), 0.0);
		color = float4(saturate(u_color.rgb * (1.0 + diffuse)), u_color.a);
	}

	return FinishWorld(pos, Transform(pos), color, uv);
}

// GLSL_MODEL_LIGHTING_VERTEX_SHADER
RR_RS
VSOut VSModelLight(float3 pos : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0)
{
	float light = u_lighting;

	if (length(u_light_dir.xyz) > 0.000001)
	{
		float4x4 m4 = mul(u_projection, u_modelview);
		// gl_Normal * mat3(m4[0].xyz, m4[1].xyz, m4[2].xyz)
		float extralight = -dot(normalize(mul(normal, (float3x3)m4)), normalize(u_light_dir.xyz));
		extralight *= u_light_contrast - u_light_backlight;
		extralight *= u_lighting / 255.0;
		light += extralight * 2.5;
	}

	light = clamp(light / 255.0, 0.0, 1.0);
	return FinishWorld(pos, Transform(pos), float4(light, light, light, 1.0), uv);
}

// A triangle covering the screen, for wipes and palette rendering's last step
RR_RS
VSOut VSFullscreen(uint id : SV_VertexID)
{
	VSOut o;
	o.pos = float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
#ifdef _UWP
	o.pos.z = u_fullscreen_depth;
	o.clipDistance = 1.0;
#endif
	o.color = float4(1.0, 1.0, 1.0, 1.0);
	o.uv = float2(0.0, 0.0);
	o.uv1 = float2(0.0, 0.0);
	o.fragz = 0.0;
	return o;
}

// Screen distortion uses clip-space vertices, independent of gameplay FOV.
RR_RS
VSOut VSPostImage(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
	return Finish(float4(pos.xy, 0.0, 1.0), u_color, uv);
}

// The second pass of DoScreenWipe: the fade mask's texture coordinates come from the corners of the screen
RR_RS
VSOut VSWipe(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
	VSOut o = Finish(Transform(pos), u_color, uv);
	o.uv1 = float2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
	return o;
}

// ==================
//  Fragment shaders
// ==================

float4 AlphaTest(float4 color)
{
	if (u_alpha_func == 1)
	{
		if (!(color.a > u_alpha_ref))
			discard;
	}
	else if (u_alpha_func == 2)
	{
		if (color.a == u_alpha_ref)
			discard;
	}
	return color;
}

// GLSL_DOOM_COLORMAP
float R_DoomColormap(float light, float z)
{
	float lightnum = clamp(light / 17.0, 0.0, 15.0);
	float lightz = clamp(z / 16.0, 0.0, 127.0);
	float startmap = (15.0 - lightnum) * 4.0;
	float scale = 160.0 / (lightz + 1.0);
	return startmap - scale * 0.5;
}

// GLSL_DOOM_LIGHT_EQUATION
float R_DoomLightingEquation(float light, float z)
{
	float colormap = floor(R_DoomColormap(light, z)) + 0.5;
	return clamp(colormap, 0.0, 31.0) / 32.0;
}

// Palette rendering, from SRB2: the palette index nearest a color
int PaletteIndex(float3 rgb)
{
	int3 c = (int3)(saturate(rgb) * 63.0 + 0.5);
	return (int)(t_palette_lookup.Load(int4(c, 0)) * 255.0 + 0.5);
}

// Palette rendering: the texel's palette index, lit through the surface's light table like the software renderer's
// colormaps. Brightmapped texels skip the surface's colormap for the base one's full-bright level, like Software's
// dc->fullbright.
float4 PaletteLight(float4 texel, float lighting, float z, float brightmap_mix)
{
	int level = brightmap_mix > 0.0 ? 32 : (int)clamp(floor(R_DoomColormap(lighting, z)), 0.0, 31.0);
	float4 color = t_lighttable.Load(int3(PaletteIndex(texel.rgb), level, 0));
	color.rgb *= u_poly_color.rgb;
	color.a = texel.a * u_poly_color.a;
	return color;
}

// GLSL_SOFTWARE_TINT_EQUATION
float4 SoftwareTint(float4 base_color, float brightmap_mix)
{
	float4 final_color = base_color;

	if (lerp(u_tint_color.a, 0.0, brightmap_mix) > 0.0)
	{
		float color_bright = sqrt((base_color.r * base_color.r) + (base_color.g * base_color.g) + (base_color.b * base_color.b));
		float strength = sqrt(9.0 * lerp(u_tint_color.a, 0.0, brightmap_mix));
		final_color.r = clamp((color_bright * (u_tint_color.r * strength)) + (base_color.r * (1.0 - strength)), 0.0, 1.0);
		final_color.g = clamp((color_bright * (u_tint_color.g * strength)) + (base_color.g * (1.0 - strength)), 0.0, 1.0);
		final_color.b = clamp((color_bright * (u_tint_color.b * strength)) + (base_color.b * (1.0 - strength)), 0.0, 1.0);
	}

	return final_color;
}

float FadeChannel(float color, float fade, float colorIntensity)
{
	if (abs(color - fade) <= colorIntensity)
		return fade;
	else if (color < fade)
		return color + colorIntensity;
	else
		return color - colorIntensity;
}

// GLSL_SOFTWARE_FADE_EQUATION
float4 SoftwareFade(float4 final_color, float final_lighting, float z)
{
	float4 fade_color = u_fade_color;
	float darkness = R_DoomLightingEquation(final_lighting, z);

	if (u_fade_start > 0.0 || u_fade_end < 31.0)
	{
		float fs = u_fade_start / 31.0;
		float fe = u_fade_end / 31.0;
		float fd = fe - fs;
		darkness = clamp((darkness - fs) * (1.0 / fd), 0.0, 1.0);
	}

	float colorBrightness = sqrt((final_color.r * final_color.r) + (final_color.g * final_color.g) + (final_color.b * final_color.b));
	float fogBrightness = sqrt((fade_color.r * fade_color.r) + (fade_color.g * fade_color.g) + (fade_color.b * fade_color.b));
	float colorIntensity = 0.0;

	if (colorBrightness < fogBrightness)
	{
		colorIntensity = 1.0 - min(final_color.r, min(final_color.g, final_color.b));
		colorIntensity = abs(colorIntensity - (1.0 - max(fade_color.r, max(fade_color.g, fade_color.b))));
	}
	else
	{
		colorIntensity = max(final_color.r, max(final_color.g, final_color.b));
		colorIntensity = abs(colorIntensity - min(fade_color.r, min(fade_color.g, fade_color.b)));
	}

	colorIntensity *= darkness;
	colorIntensity *= fade_color.a * 10.0;

	final_color.r = FadeChannel(final_color.r, fade_color.r, colorIntensity);
	final_color.g = FadeChannel(final_color.g, fade_color.g, colorIntensity);
	final_color.b = FadeChannel(final_color.b, fade_color.b, colorIntensity);
	return final_color;
}

// OpenGL's fixed function, with one texture unit
RR_RS
float4 PSFixed(VSOut i) : SV_Target
{
	float4 texel = t_tex.Sample(s_tex, i.uv);
	float4 color = i.color;

	if (u_tex_mode == 1)
		color *= texel;
	else if (u_tex_mode == 2)
		color = texel;

	return AlphaTest(color);
}

// GLSL_DEFAULT_FRAGMENT_SHADER
RR_RS
float4 PSDefault(VSOut i) : SV_Target
{
	return AlphaTest(t_tex.Sample(s_tex, i.uv) * u_poly_color);
}

// GLSL_SOFTWARE_FRAGMENT_SHADER
RR_RS
float4 PSSoftware(VSOut i) : SV_Target
{
	float4 texel = t_tex.Sample(s_tex, i.uv);
	float4 base_color = texel * u_poly_color;
	float brightmap_mix = floor(t_brightmap.Sample(s_brightmap, i.uv).r);
	float light_gain = (255.0 - u_lighting) * brightmap_mix;
	float final_lighting = u_lighting + light_gain;
	if (u_palette)
		return AlphaTest(PaletteLight(texel, final_lighting, i.fragz, brightmap_mix));
	float4 final_color = SoftwareTint(base_color, brightmap_mix);
	final_color = SoftwareFade(final_color, final_lighting, i.fragz);
	final_color.a = texel.a * u_poly_color.a;
	return AlphaTest(final_color);
}

// GLSL_SOFTWARE_MODEL_LIGHTING_FRAGMENT_SHADER
RR_RS
float4 PSSoftwareModelLight(VSOut i) : SV_Target
{
	float4 texel = t_tex.Sample(s_tex, i.uv);
	float4 base_color = texel * u_poly_color;
	float final_lighting = i.color.r * 255.0;
	float brightmap_mix = floor(t_brightmap.Sample(s_brightmap, i.uv).r);
	float light_gain = (255.0 - final_lighting) * brightmap_mix;
	final_lighting += light_gain;
	if (u_palette)
		return AlphaTest(PaletteLight(texel, final_lighting, i.fragz, brightmap_mix));
	float4 final_color = SoftwareTint(base_color, brightmap_mix);
	final_color = SoftwareFade(final_color, final_lighting, i.fragz);
	final_color.a = texel.a * u_poly_color.a;
	return AlphaTest(final_color);
}

// GLSL_WATER_FRAGMENT_SHADER
RR_RS
float4 PSWater(VSOut i) : SV_Target
{
	const float freq = 0.025;
	const float amp = 0.025;
	const float speed = 2.0;
	const float pi = 3.14159;

	float z = i.fragz / 2.0;
	float a = -pi * (z * freq) + (u_leveltime * speed);
	float sdistort = sin(a) * amp;
	float cdistort = cos(a) * amp;
	float4 texel = t_tex.Sample(s_tex, float2(i.uv.x - sdistort, i.uv.y - cdistort));
	float4 base_color = texel * u_poly_color;
	float brightmap_mix = floor(t_brightmap.Sample(s_brightmap, i.uv).r);
	float light_gain = (255.0 - u_lighting) * brightmap_mix;
	float final_lighting = u_lighting + light_gain;
	if (u_palette)
		return AlphaTest(PaletteLight(texel, final_lighting, i.fragz, brightmap_mix));
	float4 final_color = SoftwareTint(base_color, brightmap_mix);
	final_color = SoftwareFade(final_color, final_lighting, i.fragz);
	final_color.a = texel.a * u_poly_color.a;
	return AlphaTest(final_color);
}

#ifdef _UWP
Texture2D t_water_scene : register(t9);
Texture2D<int> t_water_sine : register(t10);
cbuffer WaterConstants : register(b5)
{
	float4 u_water_viewport; // left/top/right/bottom, inclusive D3D pixel coordinates
	int u_water_blend;
	int3 u_water_pad;
};

// Software's ripple phase and distance attenuation, in world units. The caller
// passes native Direct3D SV_Position.w (clip w/depth). Floor reproduces Software's
// signed fixed-point shift, including negative offsets.
float WaterBackgroundOffset(float distance, float seconds)
{
	int tick = (int)round(seconds * 35.0) % 2048;
	int angle = (tick * 140 + (int)fmod(floor(max(distance, 0.0) * 128.0), 8192.0)) & 8191;
	int wave = t_water_sine.Load(int3(angle, 0, 0));
	return floor((float)wave / (4096.0 + floor(max(distance, 0.0) * 32.0)));
}

float4 BlendWaterScene(float4 water, float4 background, int mode)
{
	float3 source = water.rgb * water.a;
	float3 result;
	if (mode == 1) result = source + background.rgb;
	else if (mode == 2) result = source - background.rgb;
	else if (mode == 3) result = background.rgb - source;
	else if (mode == 4) result = water.rgb * background.rgb;
	else if (mode == 5) result = water.rgb + background.rgb * (1.0 - water.a);
	else result = source + background.rgb * (1.0 - water.a);
	return float4(saturate(result), 1.0);
}

RR_RS
float4 PSWaterRefraction(VSOut i) : SV_Target
{
	// Keep texture/brightmap lighting and alpha-test holes from the regular shader.
	float4 water = PSWater(i);
	// Native Direct3D supplies interpolated clip w here, unlike gl_FragCoord.w.
	float2 pixel = floor(i.pos.xy);
	pixel.y += WaterBackgroundOffset(i.pos.w, u_leveltime);
	pixel = clamp(pixel, u_water_viewport.xy, u_water_viewport.zw);
	float4 background = t_water_scene.Load(int3(pixel, 0));
	// This complete composite is drawn with blending disabled, avoiding a second
	// blend against the undistorted framebuffer. Depth and stencil remain active.
	return BlendWaterScene(water, background, u_water_blend);
}
#endif

// GLSL_FOG_FRAGMENT_SHADER
RR_RS
float4 PSFog(VSOut i) : SV_Target
{
	float4 base_color = i.color;
	float brightmap_mix = 0.0;
	float final_lighting = u_lighting;
	float4 final_color = SoftwareTint(base_color, brightmap_mix);
	final_color = SoftwareFade(final_color, final_lighting, i.fragz);
	return AlphaTest(final_color);
}

// Software's integer distance/scale tables, rather than the smooth shader's
// approximation. A fog boundary remaps the scene once at its own distance.
float FogColormap(float distance, int lightnum, int plane, float viewRatio)
{
	int start = (31 - clamp(lightnum, 0, 31)) * 2;
	int attenuation;
	if (plane)
	{
		int index = min((int)floor(max(distance, 0.0) / 16.0), 127);
		attenuation = (int)floor(160.0 / (index + 1)) / 2;
	}
	else
	{
		int index = min((int)floor(5120.0 * viewRatio / max(distance, 0.0001)), 47);
		attenuation = (int)floor(index / viewRatio) / 2;
	}
	return clamp(start - attenuation, 0, 31);
}

RR_RS
float4 PSFogRemap(VSOut i) : SV_Target
{
#ifdef _UWP
	float4 scene = t_fog_scene.Load(int3(i.pos.xy, 0));
	if (u_palette)
	{
		int row = (int)FogColormap(i.pos.w, u_fog_lightnum, u_fog_plane, u_fog_view_ratio);
		return float4(t_lighttable.Load(int3(PaletteIndex(scene.rgb), row, 0)).rgb, 1.0);
	}
	// Smooth rendering retains its tint/fade equation, now applied to the scene.
	float4 color = SoftwareFade(SoftwareTint(scene, 0.0), u_lighting, i.fragz);
	return float4(color.rgb, 1.0);
#else
	return PSFog(i);
#endif
}

// GLSL_SKY_FRAGMENT_SHADER
RR_RS
float4 PSSky(VSOut i) : SV_Target
{
	return AlphaTest(t_tex.Sample(s_tex, i.uv) * i.color * u_poly_color);
}

// The second pass of DoScreenWipe: the end screen (GL_REPLACE) modulated by the fade mask on the second unit
RR_RS
float4 PSWipe(VSOut i) : SV_Target
{
	return AlphaTest(t_tex.Sample(s_tex, i.uv) * t_brightmap.Sample(s_brightmap, i.uv1));
}

// [UWP]: The RHI's rhi_glsl_fragment_postprocesswipe, with D3D's top-first screen textures.
RR_RS
float4 PSWipeFull(VSOut i) : SV_Target
{
	float2 screen = i.pos.xy / u_wipe_screen_size;
	float2 coord = float2(screen.x, 1.0 - screen.y); // GL coordinates: y=0 at the bottom
	float maskByte = t_wipe_mask.Sample(s_tex, coord) * 255.0;
	if (u_wipe_reverse)
		maskByte = 32.0 - maskByte;
	// Preserve the RHI's negative fade at reversed byte 32; only MD fades clamp it.
	float fade = 1.0 - maskByte / 31.0;
	float wiggle = (float)u_wipe_wiggle;
	float row = floor(coord.y * 200.0);
	float phase = wiggle * 128.0 + row * wiggle * 8.0;
	// GLSL mod(-row, 2) is nonnegative, unlike HLSL fmod on a negative row.
	float direction = (row - 2.0 * floor(row / 2.0)) * 2.0 - 1.0;
	screen.x += direction * sin(phase) * wiggle * 8.0 / 320.0;
	// Clamp to the captured screen, rather than the unused part of the square texture.
	float2 halfPixel = 0.5 / u_wipe_screen_size;
	float2 uv = clamp(screen, halfPixel, 1.0 - halfPixel) * u_wipe_screen_fix;
	float3 start = t_tex.Sample(s_tex, uv).rgb;
	float3 color;
	if (u_wipe_mode == 1)
		color = lerp(start, 1.0 - start, fade);
	else if (u_wipe_mode == 2)
	{
		float r = start.r - saturate(fade) * 3.0;
		float g = start.g - clamp(-r, 0.0, 3.0);
		float b = start.b - clamp(-g, 0.0, 3.0);
		color = saturate(float3(r, g, b));
	}
	else if (u_wipe_mode == 3)
	{
		float radd = saturate(fade) * 3.0;
		float gadd = max(radd - 1.0, 0.0);
		float badd = max(gadd - 1.0, 0.0);
		color = saturate(start + float3(radd, gadd, badd));
	}
	else
		color = lerp(start, t_brightmap.Sample(s_tex, uv).rgb, fade);
	return float4(color, 1.0); // Keep the screen alpha at 1, including the invert wipe.
}

// Palette rendering's last step, from SRB2: each pixel of the finished frame becomes the screen palette's color for
// its nearest palette index, which also shows palette flashes and color correction
RR_RS
float4 PSPalettePostprocess(VSOut i) : SV_Target
{
	float3 rgb = t_tex.Load(int3(i.pos.xy, 0)).rgb;
	return float4(t_screen_palette.Load(int3(PaletteIndex(rgb), 0, 0)).rgb, 1.0);
}

// Software's in-place V_OffsetIntoFadeMap remap, before the HUD and screen-palette postprocess.
RR_RS
float4 PSFadeMap(VSOut i) : SV_Target
{
	float3 rgb = t_tex.Load(int3(i.pos.xy, 0)).rgb;
	int index = PaletteIndex(rgb);
	int faded = (int)(t_fade_map.Load(int3(index, u_fade_map_row, 0)) * 255.0 + 0.5);
	return float4(t_master_palette.Load(int3(faded, 0, 0)).rgb, 1.0);
}

// Match v2.4's Software/RHI rhi_glsl_fragment_postimg, including split-view
// amplitude in full-screen UV units. The capture is square and top-first.
float2 PostImageUV(float2 uv)
{
	float2 screen = uv / u_postimg_info.zw;
	float2 low = u_postimg_rect.xy / u_postimg_info.zw;
	float2 high = u_postimg_rect.zw / u_postimg_info.zw;
	float row = (screen.y - low.y) / (high.y - low.y);
	float ticks = u_postimg_info.x;
	if (u_postimg_info.y == 1.0) // postimg_water
		screen.x += sin((ticks / 35.0) * 2.0 + row * 20.0) * 0.01;
	else if (u_postimg_info.y == 4.0) // postimg_heat
	{
		screen.x += max(sin((ticks / 35.0) * 1.2 - row * 5.0) - 0.995, 0.0);
		screen.x -= max(sin(((ticks + 24.0) / 35.0) * 0.2 - row * 1.5) - 0.990, 0.0);
	}
	return clamp(screen, low, high - 0.0001) * u_postimg_info.zw;
}

RR_RS
float4 PSPostImage(VSOut i) : SV_Target
{
	return t_tex.Sample(s_tex, PostImageUV(i.uv));
}
