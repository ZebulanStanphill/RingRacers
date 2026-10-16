// GPL-2.0; runtime translation of Legacy GL add-on GLSL.
#ifndef RR_CUSTOM_SHADER_H
#define RR_CUSTOM_SHADER_H
#include <string>
struct LegacyShaderHLSL { std::string vertex, fragment; };
// Output targets Shader Model 5.1 with all registers in space 0.
// layout: 0 polygon, 1 sky, 2 model (the driver's vertex buffer layouts).
// waterRefraction: the fragment stage also composites its alpha-tested result
// over the captured scene with ripple distortion (t9/t10/b5, as PSWaterRefraction).
// The refraction variant's vertex stage and fragment inputs are the same as the normal variant's.
bool TranslateLegacyShader(const std::string &vertex, const std::string &fragment,
    int layout, LegacyShaderHLSL &output, std::string &error, bool waterRefraction = false);
#endif
