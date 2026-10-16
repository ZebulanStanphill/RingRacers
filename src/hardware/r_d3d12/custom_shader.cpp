// GPL-2.0; runtime translation of Legacy GL add-on GLSL.
#include "custom_shader.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_hlsl.hpp>

namespace {
struct Token { size_t begin,end; std::string text; };
// Lex tokens rather than editing strings/comments or matching substrings in
// identifiers. Preserve source lines so compiler diagnostics name the add-on.
std::vector<Token> Tokens(const std::string &s) {
    std::vector<Token> t;
    for(size_t i=0;i<s.size();) {
        unsigned char c=s[i];
        if(std::isspace(c)) { ++i; continue; }
        if(c=='/' && i+1<s.size() && s[i+1]=='/') {
            i=s.find('\n',i+2); if(i==std::string::npos) break; continue;
        }
        if(c=='/' && i+1<s.size() && s[i+1]=='*') {
            size_t end=s.find("*/",i+2); if(end==std::string::npos) throw std::runtime_error("unterminated shader comment");
            i=end+2; continue;
        }
        size_t a=i++;
        if(std::isalnum(c) || c=='_') while(i<s.size() && (std::isalnum((unsigned char)s[i]) || s[i]=='_')) ++i;
        t.push_back({a,i,s.substr(a,i-a)});
    }
    return t;
}

std::string Adapt(const std::string &source, bool fragment, int layout, bool refraction=false) {
    if(source.size()>1024*1024) throw std::runtime_error("shader source exceeds 1 MB");
    const auto tokens=Tokens(source);
    const std::map<std::string,std::string> known={
        {"poly_color","vec4"},{"tint_color","vec4"},{"fade_color","vec4"},
        {"lighting","float"},{"fade_start","float"},{"fade_end","float"},
        {"light_dir","vec3"},{"light_contrast","float"},{"light_backlight","float"},{"leveltime","float"}};
    std::string body=source;
    std::vector<std::pair<Token,std::string>> edits;
    for(size_t i=0;i<tokens.size();++i) {
        const auto &t=tokens[i];
        if(t.text=="#" && i+1<tokens.size() && tokens[i+1].text=="version") {
            size_t end=source.find('\n',t.begin); if(end==std::string::npos) end=source.size();
            edits.push_back({{t.begin,end,{}},""});
            while(i+1<tokens.size() && tokens[i+1].begin<end) ++i;
        } else if(t.text=="varying") {
            edits.push_back({t,fragment?"in":"out"});
        } else if(t.text=="attribute") {
            edits.push_back({t,"in"});
        } else if(t.text=="uniform" && i+3<tokens.size()) {
            size_t end=i+1; while(end<tokens.size() && tokens[end].text!=";") ++end;
            if(end==tokens.size()) throw std::runtime_error("unterminated uniform declaration");
            size_t typeIndex=i+1;
            while(typeIndex<end && (tokens[typeIndex].text=="highp" || tokens[typeIndex].text=="mediump" || tokens[typeIndex].text=="lowp")) ++typeIndex;
            if(typeIndex+1>=end) throw std::runtime_error("invalid uniform declaration");
            const std::string type=tokens[typeIndex].text;
            std::string declarations;
            // Split comma-separated declarators only at the top level. Commas in
            // vector constructors and array initializers belong to that variable.
            size_t first=typeIndex+1;
            int nesting=0;
            for(size_t j=first;j<=end;++j) {
                const auto &part=tokens[j];
                if(part.text=="(" || part.text=="[") ++nesting;
                if(part.text==")" || part.text=="]") --nesting;
                if(j!=end && !(part.text=="," && nesting==0)) continue;
                if(j==first) throw std::runtime_error("invalid uniform declarator");
                const std::string name=tokens[first].text;
                const std::string declarator=source.substr(tokens[first].begin,tokens[j-1].end-tokens[first].begin);
                auto k=known.find(name);
                if(k!=known.end()) {
                    if(type!=k->second || (j>first+1 && tokens[first+1].text!="="))
                        throw std::runtime_error("wrong type for engine uniform "+name);
                } else if(type=="sampler2D") {
                    const int binding=name=="brightmap"?1:0;
                    declarations+="layout(set=1,binding="+std::to_string(binding)+") uniform sampler2D "+declarator+"; ";
                } else {
                    bool initializer=false;
                    for(size_t n=first+1;n<j;++n) if(tokens[n].text=="=") initializer=true;
                    if(!initializer && j!=first+1)
                        throw std::runtime_error("uninitialized custom uniform arrays require explicit GLSL initializers");
                    declarations+="const "+type+" "+declarator+(initializer?"; ":" = "+type+"(0); ");
                }
                first=j+1;
            }
            const std::string original=source.substr(t.begin,tokens[end].end-t.begin);
            declarations.append(std::count(original.begin(),original.end(),'\n'),'\n');
            edits.push_back({{t.begin,tokens[end].end,{}},declarations});
            i=end;
        }
    }
    for(auto it=edits.rbegin();it!=edits.rend();++it) body.replace(it->first.begin,it->first.end-it->first.begin,it->second);
    std::string pre=R"(#version 450
)";
    if(refraction) pre+="#extension GL_EXT_samplerless_texture_functions : require\n";
    pre+=R"(layout(std140,set=0,binding=0) uniform DrawConstants {
 mat4 rr_projection; mat4 rr_modelview;
 vec4 poly_color; vec4 tint_color; vec4 fade_color; vec4 rr_color;
 vec3 light_dir; float rr_lightdir_pad; vec4 rr_fflight;
 float lighting; float fade_start; float fade_end; float light_contrast;
 float light_backlight; float leveltime; float rr_alpha_ref; int rr_alpha_func;
 int rr_tex_mode; int rr_palette; ivec2 rr_pad;
 vec4 rr_clip_plane; float rr_fullscreen_depth; float rr_portal_pad0; float rr_portal_pad1; float rr_portal_pad2;
};
layout(std140,set=0,binding=4) uniform CustomView { vec4 rr_view; };
)";
    // Declared before the texture2D compatibility macro below.
    if(refraction) pre+=R"(layout(set=1,binding=9) uniform texture2D rr_water_scene;
layout(set=1,binding=10) uniform itexture2D rr_water_sine;
layout(std140,set=0,binding=5) uniform WaterConstants { vec4 rr_water_viewport; int rr_water_blend; };
)";
    pre+=R"(#define gl_ProjectionMatrix rr_projection
#define gl_ModelViewMatrix rr_modelview
#define gl_ModelViewProjectionMatrix (rr_projection * rr_modelview)
#define gl_NormalMatrix transpose(inverse(mat3(rr_modelview)))
#define gl_ModelViewMatrixInverse inverse(rr_modelview)
#define gl_ProjectionMatrixInverse inverse(rr_projection)
#define gl_ModelViewProjectionMatrixInverse inverse(rr_projection*rr_modelview)
#define gl_ModelViewMatrixTranspose transpose(rr_modelview)
#define gl_ProjectionMatrixTranspose transpose(rr_projection)
#define gl_ModelViewProjectionMatrixTranspose transpose(rr_projection*rr_modelview)
#define gl_ModelViewMatrixInverseTranspose transpose(inverse(rr_modelview))
#define gl_ProjectionMatrixInverseTranspose transpose(inverse(rr_projection))
const mat4 rr_texture_matrix[2]=mat4[2](mat4(1.0),mat4(1.0));
#define gl_TextureMatrix rr_texture_matrix
#define texture2D texture
#define texture2DProj textureProj
#define textureCube texture
#define main rr_user_main
)";
    if(fragment) pre+=R"(
layout(location=0) in vec4 rr_texcoord[2];
layout(location=2) in vec4 rr_frontcolor;
layout(location=0) out vec4 rr_result;
vec4 rr_fragcoord;
#define gl_TexCoord rr_texcoord
#define gl_Color rr_frontcolor
#define gl_FragColor rr_result
#define gl_FragCoord rr_fragcoord
)";
    else {
        pre+=R"(
layout(location=0) in vec3 rr_position;
layout(location=1) in vec2 rr_uv;
layout(location=0) out vec4 rr_texcoord[2];
layout(location=2) out vec4 rr_frontcolor;
vec4 rr_clipvertex;
#define gl_Vertex vec4(rr_position,1.0)
#define gl_MultiTexCoord0 vec4(rr_uv,0.0,1.0)
#define gl_MultiTexCoord1 vec4(rr_uv,0.0,1.0)
#define gl_TexCoord rr_texcoord
#define gl_FrontColor rr_frontcolor
#define gl_ClipVertex rr_clipvertex
)";
        pre+=layout==1 ? "layout(location=3) in vec4 rr_vertexcolor;\n#define gl_Color rr_vertexcolor\n" : "#define gl_Color rr_color\n";
        pre+=layout==2 ? "layout(location=2) in vec3 rr_normal;\n#define gl_Normal rr_normal\n" : "#define gl_Normal vec3(0.0,0.0,1.0)\n";
    }
    pre+="\n#line 1\n"+body+"\n#undef main\n";
    if(fragment && refraction) pre+=R"(
// shaders.hlsl's WaterBackgroundOffset and BlendWaterScene, on the same resources.
float rr_water_offset(float distance, float seconds) {
 int tick=int(round(seconds*35.0))%2048;
 int angle=(tick*140+int(mod(floor(max(distance,0.0)*128.0),8192.0)))&8191;
 int wave=texelFetch(rr_water_sine,ivec2(angle,0),0).x;
 return floor(float(wave)/(4096.0+floor(max(distance,0.0)*32.0)));
}
vec4 rr_water_composite(vec4 water, vec4 background, int mode) {
 vec3 source=water.rgb*water.a, result;
 if(mode==1) result=source+background.rgb;
 else if(mode==2) result=source-background.rgb;
 else if(mode==3) result=background.rgb-source;
 else if(mode==4) result=water.rgb*background.rgb;
 else if(mode==5) result=water.rgb+background.rgb*(1.0-water.a);
 else result=source+background.rgb*(1.0-water.a);
 return vec4(clamp(result,0.0,1.0),1.0);
}
)";
    if(fragment) pre+=R"(
#undef gl_FragCoord
void main() {
 rr_fragcoord=vec4(gl_FragCoord.x,rr_view.x-gl_FragCoord.y,gl_FragCoord.z,gl_FragCoord.w);
 rr_user_main();
 if(rr_alpha_func==1 && !(rr_result.a>rr_alpha_ref)) discard;
 if(rr_alpha_func==2 && rr_result.a==rr_alpha_ref) discard;
)";
    // Raw gl_FragCoord is Direct3D's top-left pixel center; 1/w is clip w.
    if(fragment && refraction) pre+=R"( vec2 rr_pixel=floor(gl_FragCoord.xy);
 rr_pixel.y+=rr_water_offset(1.0/gl_FragCoord.w,leveltime);
 rr_pixel=clamp(rr_pixel,rr_water_viewport.xy,rr_water_viewport.zw);
 rr_result=rr_water_composite(rr_result,texelFetch(rr_water_scene,ivec2(rr_pixel),0),rr_water_blend);
)";
    if(fragment) pre+="}\n";
    else pre+=R"(
void main() {
 rr_texcoord[0]=vec4(rr_uv,0.0,1.0); rr_texcoord[1]=rr_texcoord[0];
 rr_frontcolor=gl_Color; rr_clipvertex=rr_modelview*gl_Vertex;
 rr_user_main();
 gl_ClipDistance[0]=dot(rr_clipvertex,rr_clip_plane);
 gl_Position.z=0.5*(gl_Position.z+gl_Position.w);
}
)";
    return pre;
}

unsigned InterfaceSlots(spirv_cross::CompilerHLSL &compiler, spirv_cross::TypeID id) {
    const auto &type=compiler.get_type(id);
    unsigned count=type.columns;
    if(type.basetype==spirv_cross::SPIRType::Struct) {
        count=0; for(auto member:type.member_types) count+=InterfaceSlots(compiler,member);
    }
    for(size_t i=0;i<type.array.size();++i) {
        unsigned length=type.array_size_literal[i]?type.array[i]:compiler.get_constant(type.array[i]).scalar();
        if(length==0 || length>32) throw std::runtime_error("varying array is too large for Shader Model 5.1");
        count*=length;
    }
    if(count==0 || count>32) throw std::runtime_error("varying is too large for Shader Model 5.1");
    return count;
}

std::string HLSL(spirv_cross::CompilerHLSL &compiler, bool fragment) {
    auto hlsl=compiler.get_hlsl_options(); hlsl.shader_model=51;
    compiler.set_hlsl_options(hlsl);
    if(!fragment) {
        compiler.add_vertex_attribute_remap({0,"POSITION"});
        compiler.add_vertex_attribute_remap({1,"TEXCOORD0"});
        compiler.add_vertex_attribute_remap({2,"NORMAL"});
        compiler.add_vertex_attribute_remap({3,"COLOR0"});
    }
    const auto stage=fragment?spv::ExecutionModelFragment:spv::ExecutionModelVertex;
    for(unsigned binding : {0u,4u,5u}) {
        spirv_cross::HLSLResourceBinding map;
        map.stage=stage; map.desc_set=0; map.binding=binding; map.cbv.register_binding=binding;
        map.cbv.register_space=map.srv.register_space=map.sampler.register_space=0;
        compiler.add_hlsl_resource_binding(map);
    }
    for(unsigned binding : {0u,1u,9u,10u}) {
        spirv_cross::HLSLResourceBinding map;
        map.stage=stage; map.desc_set=1; map.binding=binding;
        map.srv.register_binding=map.sampler.register_binding=binding;
        map.cbv.register_space=map.srv.register_space=map.sampler.register_space=0;
        compiler.add_hlsl_resource_binding(map);
    }
    return compiler.compile();
}
}

bool TranslateLegacyShader(const std::string &vertex, const std::string &fragment,
    int layout, LegacyShaderHLSL &output, std::string &error, bool waterRefraction) {
    output={}; error.clear();
    try {
        if(layout<0 || layout>2) throw std::runtime_error("invalid vertex layout");
        // The renderer compiles on its main thread; initialize once per process.
        static const bool initialized=glslang::InitializeProcess();
        if(!initialized) throw std::runtime_error("glslang initialization failed");
        std::string vs=Adapt(vertex,false,layout), ps=Adapt(fragment,true,layout,waterRefraction);
        glslang::TShader v(EShLangVertex), p(EShLangFragment);
        for(auto pair : {std::make_pair(&v,&vs),std::make_pair(&p,&ps)}) {
            const char *text=pair.second->c_str(); pair.first->setStrings(&text,1);
            pair.first->setEnvInput(glslang::EShSourceGlsl,pair.first->getStage(),glslang::EShClientVulkan,450);
            pair.first->setEnvClient(glslang::EShClientVulkan,glslang::EShTargetVulkan_1_0);
            pair.first->setEnvTarget(glslang::EShTargetSpv,glslang::EShTargetSpv_1_0);
            pair.first->setAutoMapLocations(true);
            if(!pair.first->parse(GetDefaultResources(),450,false,(EShMessages)(EShMsgSpvRules|EShMsgVulkanRules)))
                throw std::runtime_error(pair.first->getInfoLog());
        }
        glslang::TProgram program; program.addShader(&v); program.addShader(&p);
        if(!program.link((EShMessages)(EShMsgSpvRules|EShMsgVulkanRules)) || !program.mapIO())
            throw std::runtime_error(program.getInfoLog());
        LegacyShaderHLSL result;
        std::vector<unsigned> vertexIR,fragmentIR;
        glslang::SpvOptions options; options.disableOptimizer=true;
        glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex),vertexIR,&options);
        glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment),fragmentIR,&options);
        spirv_cross::CompilerHLSL vertexCompiler(std::move(vertexIR)), fragmentCompiler(std::move(fragmentIR));
        // glslang's automatic Vulkan locations can collide with the reserved GL
        // compatibility interface and differ when stage declaration order differs.
        // Assign a single linked interface by name, counting arrays/matrices, and
        // keep extra vertex outputs from shifting the fragment's inputs.
        std::map<std::string,unsigned> slots,locations;
        auto gather=[&](spirv_cross::CompilerHLSL &compiler,bool fragmentStage) {
            auto resources=compiler.get_shader_resources();
            for(const auto &r: fragmentStage?resources.stage_inputs:resources.stage_outputs) {
                if(r.name=="rr_texcoord" || r.name=="rr_frontcolor") continue;
                unsigned size=InterfaceSlots(compiler,r.type_id);
                auto old=slots.find(r.name);
                if(old!=slots.end() && old->second!=size) throw std::runtime_error("incompatible varying "+r.name);
                slots[r.name]=size;
            }
        };
        gather(vertexCompiler,false); gather(fragmentCompiler,true);
        unsigned location=3; locations["rr_texcoord"]=0; locations["rr_frontcolor"]=2;
        for(const auto &slot:slots) { locations[slot.first]=location; location+=slot.second; }
        auto assign=[&](spirv_cross::CompilerHLSL &compiler,bool fragmentStage) {
            auto resources=compiler.get_shader_resources();
            for(const auto &r: fragmentStage?resources.stage_inputs:resources.stage_outputs)
                compiler.set_decoration(r.id,spv::DecorationLocation,locations.at(r.name));
        };
        assign(vertexCompiler,false); assign(fragmentCompiler,true);
        result.vertex=HLSL(vertexCompiler,false);
        result.fragment=HLSL(fragmentCompiler,true);
        output=std::move(result);
        return true;
    } catch(const std::exception &e) { error=e.what(); return false; }
}
