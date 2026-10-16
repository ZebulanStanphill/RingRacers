#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <set>
#include <sstream>
#include "hardware/r_d3d12/custom_shader.h"
#include "hardware/hw_defs.h"
#include "hardware/hw_glsl.h"
static void Write(const std::filesystem::path &dir, const std::string &name, const LegacyShaderHLSL &s) {
    if(dir.empty()) return;
    std::ofstream(dir/(name+".vert.hlsl")) << s.vertex;
    std::ofstream(dir/(name+".frag.hlsl")) << s.fragment;
}
static bool Has(const std::string &s,const char *text){return s.find(text)!=std::string::npos;}
// Check the driver's constant-buffer ABI and resource slots directly. Ignore only
// SPIR-V's generated numeric prefixes, which differ between source shaders.
static void Declarations(const std::string &source, int textures, bool refraction) {
    const std::string constants=R"(cbuffer DrawConstants : register(b0, space0)
    row_major float4x4 rr_projection : packoffset(c0);
    row_major float4x4 rr_modelview : packoffset(c4);
    float4 poly_color : packoffset(c8);
    float4 tint_color : packoffset(c9);
    float4 fade_color : packoffset(c10);
    float4 rr_color : packoffset(c11);
    float3 light_dir : packoffset(c12);
    float rr_lightdir_pad : packoffset(c12.w);
    float4 rr_fflight : packoffset(c13);
    float lighting : packoffset(c14);
    float fade_start : packoffset(c14.y);
    float fade_end : packoffset(c14.z);
    float light_contrast : packoffset(c14.w);
    float light_backlight : packoffset(c15);
    float leveltime : packoffset(c15.y);
    float rr_alpha_ref : packoffset(c15.z);
    int rr_alpha_func : packoffset(c15.w);
    int rr_tex_mode : packoffset(c16);
    int rr_palette : packoffset(c16.y);
    int2 rr_pad : packoffset(c16.z);
    float4 rr_clip_plane : packoffset(c17);
    float rr_fullscreen_depth : packoffset(c18);
    float rr_portal_pad0 : packoffset(c18.y);
    float rr_portal_pad1 : packoffset(c18.z);
    float rr_portal_pad2 : packoffset(c18.w);
cbuffer CustomView : register(b4, space0)
    float4 rr_view : packoffset(c0);
)";
    const std::string water=R"(cbuffer WaterConstants : register(b5, space0)
    float4 rr_water_viewport : packoffset(c0);
    int rr_water_blend : packoffset(c1);
)";
    auto declarations=[](const std::string &text) {
        std::multiset<std::string> result;
        std::istringstream lines(text);
        std::string buffer;
        for(std::string line;std::getline(lines,line);) {
            if(line.find("cbuffer ")==0) {
                buffer=line;
                result.insert(line);
            } else if(line.find("packoffset(")!=std::string::npos) {
                const size_t start=line.find(" _");
                if(start!=std::string::npos) {
                    const size_t prefix=start+1;
                    size_t end=prefix+1;
                    while(end<line.size() && line[end]>='0' && line[end]<='9') ++end;
                    if(end>prefix+1 && end<line.size() && line[end]=='_')
                        line.erase(prefix,end-prefix+1);
                }
                result.insert(buffer+"\n"+line);
            }
        }
        return result;
    };
    auto expected=declarations(constants+(refraction?water:""));
    auto actual=declarations(source);
    assert(actual==expected);
    expected={"register(b0, space0)","register(b4, space0)"};
    for(int slot=0;slot<textures;++slot) {
        expected.insert("register(t"+std::to_string(slot)+", space0)");
        expected.insert("register(s"+std::to_string(slot)+", space0)");
    }
    if(refraction) {
        expected.insert("register(b5, space0)");
        expected.insert("register(t9, space0)");
        expected.insert("register(t10, space0)");
    }
    actual.clear();
    for(size_t pos=0;(pos=source.find("register(",pos))!=std::string::npos;) {
        const size_t end=source.find(')',pos);
        assert(end!=std::string::npos);
        actual.insert(source.substr(pos,end-pos+1));
        pos=end+1;
    }
    // Exact sets also reject missing/duplicate slots and registers outside space 0.
    assert(actual==expected);
}
static bool Translate(const std::string &vertex, const std::string &fragment, int layout,
    LegacyShaderHLSL &result, std::string &error, bool refraction, int textures=0) {
    bool ok=TranslateLegacyShader(vertex,fragment,layout,result,error,refraction);
    if(ok) {
        Declarations(result.vertex,0,false);
        Declarations(result.fragment,textures,refraction);
    }
    return ok;
}
// The water-refraction variant must be drawable with the normal variant's
// vertex stage and must read the driver's water capture resources.
static LegacyShaderHLSL Refraction(const std::filesystem::path &dir, const std::string &name,
    const std::string &vertex, const std::string &fragment, const LegacyShaderHLSL &normal, int textures) {
    LegacyShaderHLSL result; std::string error;
    if(!Translate(vertex,fragment,0,result,error,true,textures)) fprintf(stderr,"%s: %s\n",name.c_str(),error.c_str());
    assert(error.empty());
    assert(error.empty() && result.vertex==normal.vertex && result.fragment!=normal.fragment);
    assert(Has(result.fragment,"register(t9, space0)") && Has(result.fragment,"register(t10, space0)") && Has(result.fragment,"register(b5, space0)"));
    assert(!Has(normal.fragment,"register(t9, space0)") && !Has(normal.fragment,"register(b5, space0)"));
    if(!dir.empty()) std::ofstream(dir/(name+".frag.hlsl")) << result.fragment;
    return result;
}
int main(int argc,char **argv) {
    const std::filesystem::path output=argc>1?argv[1]:"";
    if(!output.empty()) std::filesystem::create_directories(output);
    int cases=0;
    for(int type=0;type<10;++type) for(int layout=0;layout<3;++layout) {
        const int textures=type==SHADER_FOG?0:(type==SHADER_DEFAULT || type==SHADER_SKY?1:2);
        LegacyShaderHLSL result; std::string error;
        assert(Translate(gl_shadersources[type].vertex,gl_shadersources[type].fragment,layout,result,error,false,textures));
        assert(error.empty() && !result.vertex.empty() && !result.fragment.empty());
        Write(output,"builtin-"+std::to_string(type)+"-"+std::to_string(layout),result); ++cases;
        if(type==SHADER_WATER && layout==0) {
            Refraction(output,"builtin-water-refraction",gl_shadersources[type].vertex,gl_shadersources[type].fragment,result,textures); ++cases;
        }
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
        assert(Translate(vertex,fragment,layout,result,error,false,1));
        // Both stages must share the same semantics despite different ordering
        // and a vertex output that the fragment does not consume.
        assert(result.vertex.find("custom_normal : TEXCOORD3")!=std::string::npos);
        assert(result.fragment.find("custom_normal : TEXCOORD3")!=std::string::npos);
        assert(result.vertex.find("custom_uv : TEXCOORD4")!=std::string::npos);
        assert(result.fragment.find("custom_uv : TEXCOORD4")!=std::string::npos);
        Write(output,"addon-"+std::to_string(layout),result); ++cases;
        if(layout==0) { Refraction(output,"addon-refraction",vertex,fragment,result,1); ++cases; }
        // Mismatched stage interfaces and syntax errors must fail atomically.
        std::string broken=fragment;
        auto pos=broken.find("varying vec3 custom_normal");
        broken.replace(pos,std::string("varying vec3 custom_normal").size(),"varying vec4 custom_normal");
        assert(!Translate(vertex,broken,layout,result,error,false,1));
        assert(!error.empty() && result.vertex.empty() && result.fragment.empty());
        assert(!Translate(vertex,fragment,3,result,error,false,1));
        assert(!Translate(vertex,"void main() { this is invalid; }",layout,result,error,false,1));
        assert(!Translate("/* unterminated",fragment,layout,result,error,false,1));
        assert(!Translate(std::string(1024*1024+1,' '),fragment,layout,result,error,false,1));
        // An error cannot poison the compiler used for the next add-on.
        assert(Translate(vertex,fragment,layout,result,error,false,1));
        // A broken shader has no refraction variant either.
        assert(!Translate(vertex,"void main() { this is invalid; }",0,result,error,true,1));
        assert(result.vertex.empty() && result.fragment.empty() && !error.empty());
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
        assert(Translate(gl_shadersources[0].vertex,renderFragment,layout,result,error,false,0));
        Write(output,"render-"+std::to_string(layout),result);++cases;
    }
    // Custom water output composited over the refracted capture, plus a probe
    // for the depth the translated stage sees: 1/gl_FragCoord.w is clip w.
    const std::string waterFragment=R"(#version 120
uniform vec4 poly_color;
void main() {
 gl_FragColor=vec4(gl_TexCoord[0].x,0.25,0.75,poly_color.a);
}
)";
    const std::string depthFragment=R"(#version 120
void main() {
 gl_FragColor=vec4(1.0/gl_FragCoord.w,gl_FragCoord.x,gl_FragCoord.y,1.0);
}
)";
    LegacyShaderHLSL water,depth;std::string error;
    assert(Translate(gl_shadersources[0].vertex,waterFragment,0,water,error,false,0));
    Write(output,"render-water",water);++cases;
    Refraction(output,"render-water-refraction",gl_shadersources[0].vertex,waterFragment,water,0);++cases;
    assert(Translate(gl_shadersources[0].vertex,depthFragment,0,depth,error,false,0));
    assert(depth.vertex==water.vertex);
    if(!output.empty()) std::ofstream(output/"render-depth.frag.hlsl")<<depth.fragment;++cases;
    printf("SM5.1: %d complete GLSL shader/layout pairs, interface errors and compiler recovery passed\n",cases);
}
