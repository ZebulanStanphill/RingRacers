// GPL-2.0; runtime translation of Legacy GL add-on GLSL.
#ifndef RR_CUSTOM_SHADER_H
#define RR_CUSTOM_SHADER_H
#include <string>
#include <vector>
struct LegacyShaderHLSL { std::string vertex, fragment; };
// Output targets Shader Model 5.1 with all registers in space 0.
// layout: 0 polygon, 1 sky, 2 model (the driver's vertex buffer layouts).
// waterRefraction: the fragment stage also composites its alpha-tested result
// over the captured scene with ripple distortion (t9/t10/b5, as PSWaterRefraction).
// The refraction variant's vertex stage and fragment inputs are the same as the normal variant's.
bool TranslateLegacyShader(const std::string &vertex, const std::string &fragment,
    int layout, LegacyShaderHLSL &output, std::string &error, bool waterRefraction = false);

// A full-screen effect's fragment stage (VibeRant's screen effects), translated from the game's RHI GLSL sources.
// The sources are concatenated in order, as the RHI GL backend does, after "#version 450" and a few defines, so that the
// version compatibility macros pick their Vulkan-style declarations. The output targets Shader Model 5.1, space 0:
// the varyings are TEXCOORD0 (v_texcoord0) and TEXCOORD1 (v_color) by their locations; each sampler2D at
// layout(set = 0, binding = n) is Texture2D tn and SamplerState sn; the loose uniforms, which GLSL for Vulkan only
// allows in a block, are one constant buffer at b0 (constantsBytes long), in which sampler0SizeOffset is where
// u_sampler0_size's vec2 lies, in bytes, or -1 if the shader has none.
struct ScreenEffectHLSL { std::string fragment; int sampler0SizeOffset = -1; unsigned constantsBytes = 0; };
bool TranslateScreenEffect(const std::vector<std::string> &sources, ScreenEffectHLSL &output, std::string &error);
#endif
