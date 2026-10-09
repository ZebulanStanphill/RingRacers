#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "hardware/r_d3d11/custom_shader.h"
#include "hardware/hw_glsl.h"
static void Write(const std::filesystem::path &dir, const std::string &name, const LegacyShaderHLSL &s) {
    if(dir.empty()) return;
    std::ofstream(dir/(name+".vert.hlsl")) << s.vertex;
    std::ofstream(dir/(name+".frag.hlsl")) << s.fragment;
}
int main(int argc,char **argv) {
    std::filesystem::path output=argc>1?argv[1]:"";
    if(!output.empty()) std::filesystem::create_directories(output);
    int cases=0;
    for(int type=0;type<10;++type) for(int layout=0;layout<3;++layout) {
        LegacyShaderHLSL result; std::string error;
        assert(TranslateLegacyShader(gl_shadersources[type].vertex,gl_shadersources[type].fragment,layout,result,error));
        assert(error.empty() && !result.vertex.empty() && !result.fragment.empty());
        Write(output,"builtin-"+std::to_string(type)+"-"+std::to_string(layout),result); ++cases;
    }
    const std::string vertex=R"(#version 120
// uniform lighting; comments and longer identifiers must survive adaptation.
/* #version 100 */
varying vec3 custom_normal;
varying vec2 custom_uv;
varying vec4 vertex_only;
uniform float leveltime, lighting, fade_start, fade_end;
void main() {
 vertex_only=vec4(1.0);
 custom_normal=gl_Normal;
 custom_uv=gl_MultiTexCoord0.xy;
 gl_Position=gl_ModelViewProjectionMatrix*(gl_Vertex+vec4(sin(leveltime)*2.0,0.0,0.0,0.0));
 gl_FrontColor=gl_Color; gl_ClipVertex=gl_ModelViewMatrix*gl_Vertex;
}
)";
    const std::string fragment=R"(#version 120
varying vec2 custom_uv;
varying vec3 custom_normal;
uniform sampler2D tex;
uniform vec4 poly_color;
uniform float leveltime, lighting, fade_start, fade_end;
uniform float strength = 0.5, second = 0.3;
uniform vec3 default_color, other_color = vec3(0.0,0.0,0.0);
void main() {
 float lighting_extra=sin(leveltime+gl_FragCoord.z/gl_FragCoord.w);
 gl_FragColor=texture2D(tex,custom_uv)*poly_color*vec4(abs(custom_normal.xyz)+default_color,1.0);
 gl_FragColor.rgb=mix(gl_FragColor.rgb,gl_Color.rgb,strength*lighting_extra);
}
)";
    for(int layout=0;layout<3;++layout) {
        LegacyShaderHLSL result; std::string error;
        assert(TranslateLegacyShader(vertex,fragment,layout,result,error));
        // Both stages must share the same semantics despite different ordering
        // and a vertex output that the fragment does not consume.
        assert(result.vertex.find("custom_normal : TEXCOORD3")!=std::string::npos);
        assert(result.fragment.find("custom_normal : TEXCOORD3")!=std::string::npos);
        assert(result.vertex.find("custom_uv : TEXCOORD4")!=std::string::npos);
        assert(result.fragment.find("custom_uv : TEXCOORD4")!=std::string::npos);
        Write(output,"addon-"+std::to_string(layout),result); ++cases;
        // Mismatched stage interfaces and syntax errors must fail atomically.
        std::string broken=fragment;
        auto pos=broken.find("varying vec3 custom_normal");
        broken.replace(pos,std::string("varying vec3 custom_normal").size(),"varying vec4 custom_normal");
        assert(!TranslateLegacyShader(vertex,broken,layout,result,error));
        assert(!error.empty() && result.vertex.empty() && result.fragment.empty());
        assert(!TranslateLegacyShader(vertex,fragment,3,result,error));
        assert(!TranslateLegacyShader(vertex,"void main() { this is invalid; }",layout,result,error));
        assert(!TranslateLegacyShader("/* unterminated",fragment,layout,result,error));
        assert(!TranslateLegacyShader(std::string(1024*1024+1,' '),fragment,layout,result,error));
        // An error cannot poison the compiler used for the next add-on.
        assert(TranslateLegacyShader(vertex,fragment,layout,result,error));
    }
    // GPU fixtures exercise real input layouts, driver constant offsets,
    // GL fragment coordinates, alpha tests and portal clipping in Windows CI.
    const std::string renderFragment=R"(#version 120
uniform vec4 poly_color;
void main() {
 gl_FragColor=vec4(gl_TexCoord[0].x,
  (gl_TexCoord[0].y+gl_FragCoord.y/64.0)*0.5,
  gl_Color.r+gl_FragCoord.w*0.25,poly_color.a);
}
)";
    for(int layout=0;layout<3;++layout) {
        LegacyShaderHLSL result;std::string error;
        assert(TranslateLegacyShader(gl_shadersources[0].vertex,renderFragment,layout,result,error));
        Write(output,"render-"+std::to_string(layout),result);++cases;
    }
    printf("%d complete GLSL shader/layout pairs, interface errors and compiler recovery passed\n",cases);
}
