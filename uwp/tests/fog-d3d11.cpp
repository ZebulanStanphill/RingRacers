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
        const std::string good="FogColormap(i.pos.w, u_fog_lightnum";
        auto at=source.find(good);Require(at!=std::string::npos,"Cannot inject old reciprocal-depth bug");
        source.replace(at,good.size(),"FogColormap(1.0 / i.pos.w, u_fog_lightnum");
    }
    source+="\nfloat4 PSDepthProbe(VSOut i) : SV_Target { return float4(i.pos.w,i.fragz,0,1); }\n";
    auto vsCode=Compile(source,"VSPoly","vs_4_0");
    auto psCode=Compile(source,"PSFogRemap","ps_4_0");
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
    auto fogCB=buffer(16,D3D11_BIND_CONSTANT_BUFFER);
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
    int palette=1;put("u_palette",&palette,sizeof palette);
    put("u_projection",projection,sizeof projection);put("u_modelview",identity,sizeof identity);
    put("u_poly_color",white,sizeof white);put("u_color",white,sizeof white);
    put("u_clip_plane",clip,sizeof clip);put("u_lighting",&light,sizeof light);
    const UINT width=64,height=48;
    std::vector<Pixel> scenery(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        scenery[y*width+x]={((x+y)&1)?1.f:0.f,.4f,.3f,1};
    auto texture=[&](UINT w,UINT h,DXGI_FORMAT format,UINT bind,const void *data,UINT pitch) {
        D3D11_TEXTURE2D_DESC desc={};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;
        desc.SampleDesc.Count=1;desc.Format=format;desc.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA initial={};initial.pSysMem=data;initial.SysMemPitch=pitch;
        ComPtr<ID3D11Texture2D> value;Check(device->CreateTexture2D(&desc,data?&initial:nullptr,&value));return value;
    };
    auto scene=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,scenery.data(),width*sizeof(Pixel));
    std::vector<Pixel> table(256*32);
    for(int row=0;row<32;++row)for(int index=0;index<256;++index)
        table[row*256+index]={index/255.f,row/31.f,.2f,1};
    auto lighttable=texture(256,32,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,table.data(),256*sizeof(Pixel));
    std::vector<float> lookup(64*64*64);
    for(int z=0;z<64;++z)for(int y=0;y<64;++y)for(int x=0;x<64;++x)lookup[(z*64+y)*64+x]=x/63.f;
    D3D11_TEXTURE3D_DESC lookupDesc={};lookupDesc.Width=lookupDesc.Height=lookupDesc.Depth=64;
    lookupDesc.MipLevels=1;lookupDesc.Format=DXGI_FORMAT_R32_FLOAT;lookupDesc.Usage=D3D11_USAGE_DEFAULT;
    lookupDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA lookupData={lookup.data(),64*sizeof(float),64*64*sizeof(float)};
    ComPtr<ID3D11Texture3D> paletteLookup;Check(device->CreateTexture3D(&lookupDesc,&lookupData,&paletteLookup));
    auto target=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_RENDER_TARGET,nullptr,0);
    ComPtr<ID3D11RenderTargetView> rtv;Check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    D3D11_TEXTURE2D_DESC stagingDesc={};target->GetDesc(&stagingDesc);stagingDesc.BindFlags=0;
    stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&stagingDesc,nullptr,&staging));
    auto view=[&](ID3D11Texture2D *t) {ComPtr<ID3D11ShaderResourceView> v;Check(device->CreateShaderResourceView(t,nullptr,&v));return v;};
    auto sceneView=view(scene.Get()),tableView=view(lighttable.Get());
    ComPtr<ID3D11ShaderResourceView> lookupView;Check(device->CreateShaderResourceView(paletteLookup.Get(),nullptr,&lookupView));
    auto sceneSRV=sceneView.Get();context->PSSetShaderResources(8,1,&sceneSRV);
    ID3D11ShaderResourceView *paletteResources[]={lookupView.Get(),tableView.Get()};context->PSSetShaderResources(2,2,paletteResources);
    D3D11_RASTERIZER_DESC rasterDesc={};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;Check(device->CreateRasterizerState(&rasterDesc,&raster));context->RSSetState(raster.Get());
    auto rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
    context->IASetInputLayout(input.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride=sizeof(Vertex),offset=0;auto vertices=vb.Get();context->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
    context->VSSetShader(vs.Get(),nullptr,0);auto draw=drawCB.Get(),fog=fogCB.Get();
    context->VSSetConstantBuffers(0,1,&draw);context->PSSetConstantBuffers(0,1,&draw);context->PSSetConstantBuffers(3,1,&fog);
    // Distance bucket boundaries and nonconstant clip depth, all light levels,
    // both fog face kinds, full-width and half-width views, smooth tint/fade.
    const std::array<std::array<float,3>,5> distances={{{32.125f,32.125f,32.125f},
        {128.125f,128.125f,128.125f},{512.125f,512.125f,512.125f},
        {2048.125f,2048.125f,2048.125f},{16.125f,512.125f,64.125f}}};
    std::vector<float> measuredDepth(width*height);
    int pixels=0,mismatches=0,depthPixels=0;
    for(int vw:{32,64})for(int vx:{0,32})for(int vy:{0,24})for(auto d:distances) {
        if(vx+vw>(int)width)continue;
        D3D11_VIEWPORT viewport={(float)vx,(float)vy,(float)vw,24,0,1};context->RSSetViewports(1,&viewport);
        Vertex triangle[]={{-d[0],-d[0],d[0],0,0},{-d[1],3*d[1],d[1],0,0},{3*d[2],-d[2],d[2],0,0}};
        context->UpdateSubresource(vb.Get(),0,nullptr,triangle,0,0);
        // Probe once per geometry, then exercise all fog rows and smooth cases.
        for(int mode=-1;mode<67;++mode) {
            palette=mode<64?1:0;put("u_palette",&palette,sizeof palette);
            const float tint[4]={.2f,.3f,.5f,mode==66?.04f:0.f};
            const float fade[4]={0,0,0,mode==65?.1f:0.f};
            float fadeStart=0,fadeEnd=31,smoothLight=mode==65?128.f:255.f;
            put("u_lighting",&smoothLight,sizeof smoothLight);
            put("u_tint_color",tint,sizeof tint);put("u_fade_color",fade,sizeof fade);
            put("u_fade_start",&fadeStart,sizeof fadeStart);put("u_fade_end",&fadeEnd,sizeof fadeEnd);
            context->UpdateSubresource(drawCB.Get(),0,nullptr,constants.data(),0,0);
            struct FogConstants {int plane,lightnum;float ratio,pad;} fc={mode/32,mode%32,vw/(float)width,0};
            context->UpdateSubresource(fogCB.Get(),0,nullptr,&fc,0,0);
            const float sentinel[]={-1,-1,-1,-1};context->ClearRenderTargetView(rtv.Get(),sentinel);
            context->PSSetShader(mode<0?probe.Get():ps.Get(),nullptr,0);context->Draw(3,0);
            context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped={};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
                auto p=((const Pixel*)((const uint8_t*)mapped.pData+y*mapped.RowPitch))[x];
                if(x<(UINT)vx || x>=(UINT)(vx+vw) || y<(UINT)vy || y>=(UINT)vy+24) {
                    Require(p.r==-1 && p.g==-1 && p.b==-1 && p.a==-1,"Draw leaked into another view");continue;
                }
                float b2=(x-vx+.5f)/(2*vw),b1=(24-(y-vy+.5f))/48,b0=1-b1-b2;
                float distance=1/(b0/d[0]+b1/d[1]+b2/d[2]);
                if(mode<0) {
                    Require(fabs(p.r-distance)<distance*.00001f && fabs(p.g-distance*.5f)<distance*.00001f,"D3D position.w must equal interpolated clip depth");
                    measuredDepth[y*width+x]=p.r;++depthPixels;
                } else {
                    auto expected=scenery[y*width+x];float z=measuredDepth[y*width+x];
                    if(mode<64) {
                        // Independent Software zlight/scalelight integer oracle.
                        int index,attenuation;
                        if(fc.plane){index=std::min((int)(z/16),127);attenuation=(160/(index+1))/2;}
                        else{index=std::min((int)(5120*fc.ratio/z),47);attenuation=(int)(index/fc.ratio)/2;}
                        int row=std::clamp((31-fc.lightnum)*2-attenuation,0,31);
                        expected=table[row*256+(expected.r==1?255:0)];
                    } else if(mode==65) {
                        float lightz=std::clamp(z*.5f/16,0.f,127.f);
                        float darkness=std::clamp(floorf((15-128.f/17)*4-80/(lightz+1))+.5f,0.f,31.f)/32;
                        float intensity=std::max(expected.r,std::max(expected.g,expected.b))*darkness;
                        expected.r=std::max(0.f,expected.r-intensity);expected.g=std::max(0.f,expected.g-intensity);expected.b=std::max(0.f,expected.b-intensity);
                    } else if(mode==66) {
                        float brightness=sqrtf(expected.r*expected.r+expected.g*expected.g+expected.b*expected.b),strength=.6f;
                        expected.r=std::clamp(brightness*tint[0]*strength+expected.r*(1-strength),0.f,1.f);
                        expected.g=std::clamp(brightness*tint[1]*strength+expected.g*(1-strength),0.f,1.f);
                        expected.b=std::clamp(brightness*tint[2]*strength+expected.b*(1-strength),0.f,1.f);
                    }
                    bool match=fabs(p.r-expected.r)<1e-5f && fabs(p.g-expected.g)<1e-5f && fabs(p.b-expected.b)<1e-5f && p.a==1;
                    if(!match) {
                        if(!mismatches)fprintf(stderr,"First mismatch at (%u,%u), mode %d, depth %.8g: expected green %.8g, got %.8g\n",x,y,mode,z,expected.g,p.g);
                        ++mismatches;
                    }
                    ++pixels;
                }
            }
            context->Unmap(staging.Get(),0);
        }
    }
    if(negative)Require(mismatches>0,"Reciprocal-depth fog shader unexpectedly passed");
    else Require(mismatches==0,"Production fog shader remapped the wrong scenery colors");
    printf("SM4 D3D11 WARP: %d depth pixels verified; %d fog composites, %d mismatches%s\n",depthPixels,pixels,mismatches,negative?" (reciprocal depth correctly rejected)":"");
}
