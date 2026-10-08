// Exercise the actual SM4 shaders through Direct3D's rasterizer, not a mock of
// pixel-shader inputs. WARP makes the test independent of the runner's GPU.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
using fixed_t=int32_t;
#include "t_fsin.c"

void Check(HRESULT result) {
    if(FAILED(result)) { fprintf(stderr,"Direct3D failed: 0x%08lx\n",(unsigned long)result);exit(1); }
}
void Require(bool value,const char *message) {
    if(!value) { fprintf(stderr,"%s\n",message);exit(1); }
}
ComPtr<ID3DBlob> Compile(const std::string &source,const char *entry,const char *target) {
    ComPtr<ID3DBlob> code,error;
    const D3D_SHADER_MACRO macros[]={{"_UWP","1"},{nullptr,nullptr}};
    HRESULT result=D3DCompile(source.data(),source.size(),"production shaders.hlsl",macros,nullptr,
        entry,target,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(error)fprintf(stderr,"%s",(const char*)error->GetBufferPointer());Check(result);return code;
}
struct Pixel { float r,g,b,a; };
struct Vertex { float x,y,z,u,v; };
int main(int argc,char **argv) {
    Require(argc==2 || argc==3,"Pass the patched engine directory and optional --negative-control");
    std::ifstream file(std::string(argv[1])+"/src/hardware/r_d3d11/shaders.hlsl");
    Require((bool)file,"Cannot read production shaders");
    std::string source((std::istreambuf_iterator<char>(file)),{});
    bool negative=argc==3 && std::string(argv[2])=="--negative-control";
    if(negative) {
        const std::string good="WaterBackgroundOffset(i.pos.w, u_leveltime)";
        auto at=source.find(good);Require(at!=std::string::npos,"Cannot inject old reciprocal-depth bug");
        source.replace(at,good.size(),"WaterBackgroundOffset(1.0 / i.pos.w, u_leveltime)");
    }
    source+="\nfloat4 PSDepthProbe(VSOut i) : SV_Target { return float4(i.pos.w,i.fragz,0,1); }\n";
    auto vsCode=Compile(source,"VSPoly","vs_4_0");
    auto psCode=Compile(source,"PSWaterRefraction","ps_4_0");
    auto probeCode=Compile(source,"PSDepthProbe","ps_4_0");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_10_1,actual;
    Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&requested,1,D3D11_SDK_VERSION,&device,&actual,&context));
    Require(actual==requested,"Test must run at feature level 10.1 / Shader Model 4");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps,probe;
    Check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));
    Check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    Check(device->CreatePixelShader(probeCode->GetBufferPointer(),probeCode->GetBufferSize(),nullptr,&probe));
    const D3D11_INPUT_ELEMENT_DESC layout[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> input;
    Check(device->CreateInputLayout(layout,2,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&input));
    auto buffer=[&](UINT size,UINT bind) {
        D3D11_BUFFER_DESC desc={};desc.ByteWidth=size;desc.BindFlags=bind;desc.Usage=D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Buffer> value;Check(device->CreateBuffer(&desc,nullptr,&value));return value;
    };
    auto vb=buffer(3*sizeof(Vertex),D3D11_BIND_VERTEX_BUFFER);
    auto waterCB=buffer(32,D3D11_BIND_CONSTANT_BUFFER);
    // Reflect the actual b0 layout; keep offsets independent of a copied C++ struct.
    ComPtr<ID3D11ShaderReflection> reflection;
    Check(D3DReflect(psCode->GetBufferPointer(),psCode->GetBufferSize(),IID_ID3D11ShaderReflection,&reflection));
    auto cb=reflection->GetConstantBufferByName("DrawConstants");D3D11_SHADER_BUFFER_DESC cbDesc={};Check(cb->GetDesc(&cbDesc));
    std::vector<uint8_t> constants(cbDesc.Size,0);auto drawCB=buffer(cbDesc.Size,D3D11_BIND_CONSTANT_BUFFER);
    auto put=[&](const char *name,const void *data,UINT size) {
        D3D11_SHADER_VARIABLE_DESC variable={};Check(cb->GetVariableByName(name)->GetDesc(&variable));
        Require(size<=variable.Size && variable.StartOffset+size<=constants.size(),"Constant exceeds reflected field");
        memcpy(constants.data()+variable.StartOffset,data,size);
    };
    float identity[16]={};for(int i=0;i<4;++i)identity[i*5]=1;
    float projection[16]={};projection[0]=projection[5]=projection[11]=1; // clip w = vertex z
    const float white[4]={1,1,1,1},clip[4]={0,0,0,1},light=255;
    put("u_projection",projection,sizeof projection);put("u_modelview",identity,sizeof identity);
    put("u_poly_color",white,sizeof white);put("u_color",white,sizeof white);
    put("u_clip_plane",clip,sizeof clip);put("u_lighting",&light,sizeof light);
    const UINT width=64,height=48;
    std::vector<Pixel> scenery(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        scenery[y*width+x]={(float)x/width,(float)y/height,y<height/2?.9f:.1f,1};
    auto texture=[&](UINT w,UINT h,DXGI_FORMAT format,UINT bind,const void *data,UINT pitch) {
        D3D11_TEXTURE2D_DESC desc={};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;
        desc.SampleDesc.Count=1;desc.Format=format;desc.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA initial={};initial.pSysMem=data;initial.SysMemPitch=pitch;
        ComPtr<ID3D11Texture2D> value;Check(device->CreateTexture2D(&desc,data?&initial:nullptr,&value));return value;
    };
    auto scene=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,scenery.data(),width*sizeof(Pixel));
    auto sine=texture(8192,1,DXGI_FORMAT_R32_SINT,D3D11_BIND_SHADER_RESOURCE,finesine,8192*sizeof(fixed_t));
    const Pixel clearTex={0,0,0,0};
    auto transparent=texture(1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,&clearTex,sizeof clearTex);
    auto target=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_RENDER_TARGET,nullptr,0);
    ComPtr<ID3D11RenderTargetView> rtv;Check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    D3D11_TEXTURE2D_DESC stagingDesc={};target->GetDesc(&stagingDesc);stagingDesc.BindFlags=0;
    stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&stagingDesc,nullptr,&staging));
    auto view=[&](ID3D11Texture2D *t) {ComPtr<ID3D11ShaderResourceView> v;Check(device->CreateShaderResourceView(t,nullptr,&v));return v;};
    auto sceneView=view(scene.Get()),sineView=view(sine.Get()),texView=view(transparent.Get());
    ID3D11ShaderResourceView *surfaces[]={texView.Get(),texView.Get()},*background[]={sceneView.Get(),sineView.Get()};
    context->PSSetShaderResources(0,2,surfaces);context->PSSetShaderResources(9,2,background);
    D3D11_SAMPLER_DESC samplerDesc={};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;Check(device->CreateSamplerState(&samplerDesc,&sampler));
    ID3D11SamplerState *samplers[]={sampler.Get(),sampler.Get()};context->PSSetSamplers(0,2,samplers);
    D3D11_RASTERIZER_DESC rasterDesc={};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;Check(device->CreateRasterizerState(&rasterDesc,&raster));context->RSSetState(raster.Get());
    auto rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
    context->IASetInputLayout(input.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride=sizeof(Vertex),offset=0;auto vertices=vb.Get();context->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
    context->VSSetShader(vs.Get(),nullptr,0);auto draw=drawCB.Get(),water=waterCB.Get();
    context->VSSetConstantBuffers(0,1,&draw);context->PSSetConstantBuffers(0,1,&draw);context->PSSetConstantBuffers(5,1,&water);
    const std::array<std::array<float,3>,6> distances={{{.125f,.125f,.125f},{32.125f,32.125f,32.125f},
        {128.125f,128.125f,128.125f},{512.125f,512.125f,512.125f},{2048.125f,2048.125f,2048.125f},{16.125f,512.125f,64.125f}}};
    std::vector<float> measuredDepth(width*height);
    int pixels=0,mismatches=0,depthPixels=0;
    for(int vx:{0,32})for(int vy:{0,24})for(auto d:distances)for(int tick:{0,6,41,128}) {
        D3D11_VIEWPORT viewport={(float)vx,(float)vy,32,24,0,1};context->RSSetViewports(1,&viewport);
        float time=tick/35.f;put("u_leveltime",&time,sizeof time);context->UpdateSubresource(drawCB.Get(),0,nullptr,constants.data(),0,0);
        struct WaterConstants {float bounds[4];int mode,pad[3];} wc={{(float)vx,(float)vy,(float)vx+31,(float)vy+23},0,{0,0,0}};
        context->UpdateSubresource(waterCB.Get(),0,nullptr,&wc,0,0);
        Vertex triangle[]={{-d[0],-d[0],d[0],0,0},{-d[1],3*d[1],d[1],0,0},{3*d[2],-d[2],d[2],0,0}};
        context->UpdateSubresource(vb.Get(),0,nullptr,triangle,0,0);
        for(bool depth:{true,false}) {
            const float sentinel[]={-1,-1,-1,-1};context->ClearRenderTargetView(rtv.Get(),sentinel);
            context->PSSetShader(depth?probe.Get():ps.Get(),nullptr,0);context->Draw(3,0);
            context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped={};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
                auto p=((const Pixel*)((const uint8_t*)mapped.pData+y*mapped.RowPitch))[x];
                if(x<(UINT)vx || x>=(UINT)vx+32 || y<(UINT)vy || y>=(UINT)vy+24) {
                    Require(p.r==-1 && p.g==-1 && p.b==-1 && p.a==-1,"Draw leaked into another view");continue;
                }
                float b2=(x-vx+.5f)/64,b1=(24-(y-vy+.5f))/48,b0=1-b1-b2;
                float distance=1/(b0/d[0]+b1/d[1]+b2/d[2]);
                if(depth) {
                    Require(fabs(p.r-distance)<distance*.00001f && fabs(p.g-distance*.5f)<distance*.00001f,"Native D3D PS position.w must equal interpolated clip depth");
                    measuredDepth[y*width+x]=p.r;++depthPixels;
                } else {
                    // Independent fixed-point sine/attenuation oracle, using
                    // the measured rasterizer depth to avoid CPU/GPU rounding.
                    int fx=(int)(measuredDepth[y*width+x]*65536),angle=(tick*140+(fx>>9))&8191;
                    int shift=(int)floor((double)finesine[angle]/(4096+(fx>>11)));
                    int sy=std::clamp((int)y+shift,vy,vy+23);auto expected=scenery[sy*width+x];
                    bool match=fabs(p.r-expected.r)<1e-5f && fabs(p.g-expected.g)<1e-5f && fabs(p.b-expected.b)<1e-5f && p.a==1;
                    if(!match) {
                        if(!mismatches)fprintf(stderr,"First mismatch at (%u,%u), tick %d, depth %.8g: expected row %d / %.8g, got %.8g\n",x,y,tick,measuredDepth[y*width+x],sy,expected.g,p.g);
                        ++mismatches;
                    }
                    ++pixels;
                }
            }
            context->Unmap(staging.Get(),0);
        }
    }
    if(negative)Require(mismatches>0,"Old reciprocal-depth shader unexpectedly passed");
    else Require(mismatches==0,"Production water shader sampled the wrong scenery rows");
    printf("SM4 D3D11 WARP: %d depth pixels verified; %d water composites, %d mismatches%s\n",depthPixels,pixels,mismatches,negative?" (old shader correctly rejected)":"");
}
