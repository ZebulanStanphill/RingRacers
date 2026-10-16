#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "hardware/r_d3d12/custom_shader.h"
#include "hardware/hw_defs.h"
#include "hardware/hw_glsl.h"
static void Write(const std::filesystem::path &dir, const std::string &name, const LegacyShaderHLSL &s) {
    if(dir.empty()) return;
    std::ofstream(dir/(name+".vert.hlsl")) << s.vertex;
    std::ofstream(dir/(name+".frag.hlsl")) << s.fragment;
}
static bool Has(const std::string &s,const char *text){return s.find(text)!=std::string::npos;}
// Explicit space 0 must preserve all resource slots and std140 pack offsets.
static void Compatible(const std::string &sm40, const std::string &sm51) {
    std::string normalized=sm51;
    for(size_t pos=0;(pos=normalized.find(", space0",pos))!=std::string::npos;)
        normalized.erase(pos,8);
    auto declarations=[](const std::string &source) {
        std::string result;
        for(size_t start=0;start<source.size();) {
            size_t end=source.find('\n',start);
            if(end==std::string::npos) end=source.size();
            std::string line=source.substr(start,end-start);
            const size_t slot=line.find("register(");
            if(line.find("packoffset(")!=std::string::npos || line.find("cbuffer ")==0)
                result+=line+"\n";
            else if(slot!=std::string::npos)
                result+=line.substr(slot,line.find(')',slot)-slot+1)+"\n";
            start=end+1;
        }
        return result;
    };
    assert(declarations(sm40)==declarations(normalized));
    for(size_t pos=0;(pos=sm51.find("register(",pos))!=std::string::npos;) {
        const size_t end=sm51.find(')',pos);
        assert(end!=std::string::npos);
        assert(sm51.substr(pos,end-pos).find(", space0")!=std::string::npos);
        pos=end+1;
    }
}
static bool Translate(const std::string &vertex, const std::string &fragment, int layout,
    LegacyShaderHLSL &result, std::string &error, bool refraction, int shaderModel) {
    bool ok=TranslateLegacyShader(vertex,fragment,layout,result,error,refraction,shaderModel);
    if(ok && shaderModel==51) {
        LegacyShaderHLSL sm40; std::string sm40Error;
        assert(TranslateLegacyShader(vertex,fragment,layout,sm40,sm40Error,refraction));
        Compatible(sm40.vertex,result.vertex); Compatible(sm40.fragment,result.fragment);
    }
    return ok;
}
// The water-refraction variant must be drawable with the normal variant's
// vertex stage and must read the driver's water capture resources.
static LegacyShaderHLSL Refraction(const std::filesystem::path &dir, const std::string &name,
    const std::string &vertex, const std::string &fragment, const LegacyShaderHLSL &normal, int shaderModel) {
    LegacyShaderHLSL result; std::string error;
    if(!Translate(vertex,fragment,0,result,error,true,shaderModel)) fprintf(stderr,"%s: %s\n",name.c_str(),error.c_str());
    assert(error.empty());
    assert(error.empty() && result.vertex==normal.vertex && result.fragment!=normal.fragment);
    assert(Has(result.fragment,(shaderModel==51?"register(t9, space0)":"register(t9)")) && Has(result.fragment,(shaderModel==51?"register(t10, space0)":"register(t10)")) && Has(result.fragment,(shaderModel==51?"register(b5, space0)":"register(b5)")));
    assert(!Has(normal.fragment,(shaderModel==51?"register(t9, space0)":"register(t9)")) && !Has(normal.fragment,(shaderModel==51?"register(b5, space0)":"register(b5)")));
    if(!dir.empty()) std::ofstream(dir/(name+".frag.hlsl")) << result.fragment;
    return result;
}
int main(int argc,char **argv) {
    const std::filesystem::path requestedOutput=argc>1?argv[1]:"";
    for(int shaderModel : {40,51}) {
    std::filesystem::path output=requestedOutput;
    if(shaderModel==51 && !output.empty()) output=output.parent_path()/"translated-shaders-sm51";
    if(!output.empty()) std::filesystem::create_directories(output);
    int cases=0;
    for(int type=0;type<10;++type) for(int layout=0;layout<3;++layout) {
        LegacyShaderHLSL result; std::string error;
        assert(Translate(gl_shadersources[type].vertex,gl_shadersources[type].fragment,layout,result,error,false,shaderModel));
        assert(error.empty() && !result.vertex.empty() && !result.fragment.empty());
        Write(output,"builtin-"+std::to_string(type)+"-"+std::to_string(layout),result); ++cases;
        if(type==SHADER_WATER && layout==0) {
            Refraction(output,"builtin-water-refraction",gl_shadersources[type].vertex,gl_shadersources[type].fragment,result,shaderModel); ++cases;
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
        assert(Translate(vertex,fragment,layout,result,error,false,shaderModel));
        // Both stages must share the same semantics despite different ordering
        // and a vertex output that the fragment does not consume.
        assert(result.vertex.find("custom_normal : TEXCOORD3")!=std::string::npos);
        assert(result.fragment.find("custom_normal : TEXCOORD3")!=std::string::npos);
        assert(result.vertex.find("custom_uv : TEXCOORD4")!=std::string::npos);
        assert(result.fragment.find("custom_uv : TEXCOORD4")!=std::string::npos);
        Write(output,"addon-"+std::to_string(layout),result); ++cases;
        if(layout==0) { Refraction(output,"addon-refraction",vertex,fragment,result,shaderModel); ++cases; }
        // Mismatched stage interfaces and syntax errors must fail atomically.
        std::string broken=fragment;
        auto pos=broken.find("varying vec3 custom_normal");
        broken.replace(pos,std::string("varying vec3 custom_normal").size(),"varying vec4 custom_normal");
        assert(!Translate(vertex,broken,layout,result,error,false,shaderModel));
        assert(!error.empty() && result.vertex.empty() && result.fragment.empty());
        assert(!Translate(vertex,fragment,3,result,error,false,shaderModel));
        assert(!Translate(vertex,"void main() { this is invalid; }",layout,result,error,false,shaderModel));
        assert(!Translate("/* unterminated",fragment,layout,result,error,false,shaderModel));
        assert(!Translate(std::string(1024*1024+1,' '),fragment,layout,result,error,false,shaderModel));
        // An error cannot poison the compiler used for the next add-on.
        assert(Translate(vertex,fragment,layout,result,error,false,shaderModel));
        // A broken shader has no refraction variant either.
        assert(!Translate(vertex,"void main() { this is invalid; }",0,result,error,true,shaderModel));
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
        assert(Translate(gl_shadersources[0].vertex,renderFragment,layout,result,error,false,shaderModel));
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
    assert(Translate(gl_shadersources[0].vertex,waterFragment,0,water,error,false,shaderModel));
    Write(output,"render-water",water);++cases;
    Refraction(output,"render-water-refraction",gl_shadersources[0].vertex,waterFragment,water,shaderModel);++cases;
    assert(Translate(gl_shadersources[0].vertex,depthFragment,0,depth,error,false,shaderModel));
    assert(depth.vertex==water.vertex);
    if(!output.empty()) std::ofstream(output/"render-depth.frag.hlsl")<<depth.fragment;++cases;
    printf("SM%d: %d complete GLSL shader/layout pairs, interface errors and compiler recovery passed\n",shaderModel,cases);
    }
}
