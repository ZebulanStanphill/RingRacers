// Exercise the actual SM4 palette shader through Direct3D's rasterizer. Brightmapped
// palette texels must use the base colormap's full-bright row (32), while other texels
// use the surface colormap row chosen by R_DoomColormap at the rasterized depth.
// WARP makes the test independent of the runner's GPU.
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
#include <cstring>
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

// The surface colormap of GLSL_DOOM_COLORMAP, as the shader computes it.
float DoomColormap(float light,float z) {
    float lightnum=std::clamp(light/17.f,0.f,15.f);
    float lightz=std::clamp(z/16.f,0.f,127.f);
    float startmap=(15.f-lightnum)*4.f;
    float scale=160.f/(lightz+1.f);
    return startmap-scale*.5f;
}
int ExpectedRow(float light,float z) {
    return (int)std::clamp(std::floor(DoomColormap(light,z)),0.f,31.f);
}

int main(int argc,char **argv) {
    Require(argc==2 || argc==3,"Pass the patched engine directory and optional --negative-control");
    std::ifstream file(std::string(argv[1])+"/src/hardware/r_d3d11/shaders.hlsl");
    Require((bool)file,"Cannot read production shaders");
    std::string source((std::istreambuf_iterator<char>(file)),{});
    bool negative=argc==3 && std::string(argv[2])=="--negative-control";
    if(negative) {
        // The old palette path ignored the brightmap and used the surface colormap's row.
        const std::string good="brightmap_mix > 0.0 ? 32 : ";
        auto at=source.find(good);Require(at!=std::string::npos,"Cannot inject old brightmap light-table bug");
        source.erase(at,good.size());
    }
    source+="\nfloat4 PSDepthProbe(VSOut i) : SV_Target { return float4(i.pos.w,i.fragz,0,1); }\n";
    auto vsCode=Compile(source,"VSPoly","vs_4_0");
    auto psCode=Compile(source,"PSSoftware","ps_4_0");
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
    // Identity matrices with clip w equal to the vertex z, so fragz is half the depth.
    float identity[16]={};for(int i=0;i<4;++i)identity[i*5]=1;
    float projection[16]={};projection[0]=projection[5]=projection[11]=1;
    const float white[4]={1,1,1,1},clip[4]={0,0,0,1},fadeEnd=31;
    int palette=1,alphaFunc=0;
    put("u_palette",&palette,sizeof palette);put("u_alpha_func",&alphaFunc,sizeof alphaFunc);
    put("u_projection",projection,sizeof projection);put("u_modelview",identity,sizeof identity);
    put("u_poly_color",white,sizeof white);put("u_color",white,sizeof white);
    put("u_clip_plane",clip,sizeof clip);put("u_fade_end",&fadeEnd,sizeof fadeEnd);
    const UINT width=16,height=8;
    auto texture=[&](UINT w,UINT h,DXGI_FORMAT format,UINT bind,const void *data,UINT pitch) {
        D3D11_TEXTURE2D_DESC desc={};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;
        desc.SampleDesc.Count=1;desc.Format=format;desc.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA initial={};initial.pSysMem=data;initial.SysMemPitch=pitch;
        ComPtr<ID3D11Texture2D> value;Check(device->CreateTexture2D(&desc,data?&initial:nullptr,&value));return value;
    };
    // Four test colors on exact cells of the 64^3 lookup, each with its own palette index.
    const int cells[4][3]={{0,0,0},{63,63,63},{63,0,0},{0,0,63}};
    const int paletteIndices[4]={0,255,128,37};
    // Texel k is (x+2y)&3; brightmap is 1 where ((x>>2)+y) is odd, so every color has both states.
    std::vector<Pixel> texels(width*height),brightmaps(width*height);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        UINT k=(x+2*y)&3;
        texels[y*width+x]={cells[k][0]/63.f,cells[k][1]/63.f,cells[k][2]/63.f,1.f};
        float bright=(((x>>2)+y)&1)!=0 ? 1.f : 0.f;
        brightmaps[y*width+x]={bright,bright,bright,1.f};
    }
    const UINT pitch=static_cast<UINT>(width*sizeof(Pixel));
    auto texture2D=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,texels.data(),pitch);
    auto brightTexture=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,brightmaps.data(),pitch);
    // Light table: rows 0-31 are (index, row/32, .5); row 32 is the full-bright row (index, 1, 1).
    std::vector<Pixel> table(256*33);
    for(int row=0;row<33;++row)for(int index=0;index<256;++index)
        table[row*256+index]=row<32 ? Pixel{index/255.f,row/32.f,.5f,1.f} : Pixel{index/255.f,1.f,1.f,1.f};
    auto lighttable=texture(256,33,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,table.data(),static_cast<UINT>(256*sizeof(Pixel)));
    // Every cell not listed maps to an unused index, so a misplaced lookup would be visible.
    std::vector<float> lookup(64*64*64,99.f/255.f);
    for(int n=0;n<4;++n)lookup[(cells[n][2]*64+cells[n][1])*64+cells[n][0]]=paletteIndices[n]/255.f;
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
    auto texView=view(texture2D.Get()),brightView=view(brightTexture.Get()),tableView=view(lighttable.Get());
    ComPtr<ID3D11ShaderResourceView> lookupView;Check(device->CreateShaderResourceView(paletteLookup.Get(),nullptr,&lookupView));
    ID3D11ShaderResourceView *surfaces[]={texView.Get(),brightView.Get()};context->PSSetShaderResources(0,2,surfaces);
    ID3D11ShaderResourceView *paletteResources[]={lookupView.Get(),tableView.Get()};context->PSSetShaderResources(2,2,paletteResources);
    D3D11_SAMPLER_DESC samplerDesc={};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;Check(device->CreateSamplerState(&samplerDesc,&sampler));
    ID3D11SamplerState *samplers[]={sampler.Get(),sampler.Get()};context->PSSetSamplers(0,2,samplers);
    D3D11_RASTERIZER_DESC rasterDesc={};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;Check(device->CreateRasterizerState(&rasterDesc,&raster));context->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport={0.f,0.f,(float)width,(float)height,0.f,1.f};context->RSSetViewports(1,&viewport);
    auto rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
    context->IASetInputLayout(input.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride=sizeof(Vertex),offset=0;auto vertices=vb.Get();context->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
    context->VSSetShader(vs.Get(),nullptr,0);auto draw=drawCB.Get();
    context->VSSetConstantBuffers(0,1,&draw);context->PSSetConstantBuffers(0,1,&draw);
    // Copy the target to the CPU, returning pixels in row-major order.
    auto readback=[&]() {
        context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped={};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::vector<Pixel> out(width*height);
        for(UINT y=0;y<height;++y) {
            auto row=(const Pixel*)((const uint8_t*)mapped.pData+y*mapped.RowPitch);
            for(UINT x=0;x<width;++x)out[y*width+x]=row[x];
        }
        context->Unmap(staging.Get(),0);
        return out;
    };
    // Full-screen triangle at constant clip depth, so each pixel samples one texel exactly.
    // Light levels and depths cover the clamp at both ends and rows in between.
    const std::array<float,2> lightings={{64.f,160.f}};
    const std::array<float,7> depths={{16.f,48.f,64.f,96.f,128.f,180.f,256.f}};
    const float sentinel[]={-1,-1,-1,-1};
    const float tolerance=1.f/512.f;
    int brightPixels=0,surfacePixels=0,mismatches=0;
    for(float lighting:lightings) {
        put("u_lighting",&lighting,sizeof lighting);
        for(float depth:depths) {
            Vertex triangle[]={{-depth,-depth,depth,0.f,1.f},{-depth,3*depth,depth,0.f,-1.f},{3*depth,-depth,depth,2.f,1.f}};
            context->UpdateSubresource(vb.Get(),0,nullptr,triangle,0,0);
            context->UpdateSubresource(drawCB.Get(),0,nullptr,constants.data(),0,0);
            // Measure the rasterized fragz first; the surface colormap uses it.
            context->ClearRenderTargetView(rtv.Get(),sentinel);
            context->PSSetShader(probe.Get(),nullptr,0);context->Draw(3,0);
            auto probed=readback();
            for(const Pixel &p:probed)
                Require(std::fabs(p.g-depth*.5f)<=depth*1e-5f,"Rasterized fragz must equal half the constant clip depth");
            context->ClearRenderTargetView(rtv.Get(),sentinel);
            context->PSSetShader(ps.Get(),nullptr,0);context->Draw(3,0);
            auto actual=readback();
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
                size_t i=y*width+x;
                UINT k=(x+2*y)&3;
                int index=paletteIndices[k];
                bool bright=(((x>>2)+y)&1)!=0;
                int row=bright ? 32 : ExpectedRow(lighting,probed[i].g);
                Pixel expected=table[static_cast<size_t>(row*256+index)];
                Pixel p=actual[i];
                bool match=std::fabs(p.r-expected.r)<tolerance && std::fabs(p.g-expected.g)<tolerance &&
                    std::fabs(p.b-expected.b)<tolerance && std::fabs(p.a-1.f)<tolerance;
                if(!match) {
                    if(!mismatches)fprintf(stderr,"First mismatch at (%u,%u), brightmap %d, lighting %g, depth %g: expected row %d (%g,%g,%g), got (%g,%g,%g,%g)\n",
                        x,y,bright?1:0,lighting,depth,row,expected.r,expected.g,expected.b,p.r,p.g,p.b,p.a);
                    ++mismatches;
                }
                if(bright)++brightPixels;else ++surfacePixels;
            }
        }
    }
    if(negative)Require(mismatches>0,"Old surface-colormap brightmap row unexpectedly passed");
    else Require(mismatches==0,"Production palette shader used the wrong brightmap light-table row");
    printf("SM4 D3D11 WARP: %d brightmap pixels on row 32, %d surface-colormap pixels verified; %d mismatches%s\n",
        brightPixels,surfacePixels,mismatches,negative?" (old brightmap row correctly rejected)":"");
}
