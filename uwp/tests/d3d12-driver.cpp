// Runs the production driver and GPU code; only engine services and ANGLE presentation are stubbed.
#include "hardware/r_d3d12/d3d12_core.h"
#include "hardware/r_d3d12/d3d12_present.h"
#include "hardware/r_d3d12/d3d12_prewarm.h"
#include "doomdef.h"
#include "command.h"
#include "i_system.h"
#include "hardware/hw_drv.h"
#include "hardware/hw_main.h"
#include "hardware/r_d3d12/r_d3d12.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#undef min
#undef max
#undef clamp
using namespace rr_d3d12;
static_assert((PF_Invert & PF_Occlude) == 0, "Inversion must not alias depth writes");
static std::string logs, negative;
static bool negativeRejected=false;
extern "C" {
consvar_t cv_gld3d12{}, cv_vidwait{}, cv_reducevfx{};
CV_PossibleValue_t glanisotropicmode_cons_t[] = {{0, "Off"}, {16, "16"}, {0, nullptr}};
void HWR_Startup() {}
void CV_StealthSet(consvar_t *, const char *) {}
void M_SaveConfig(const char *) {}
precise_t I_GetPreciseTime() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
UINT64 I_GetPrecisePrecision() { LARGE_INTEGER n; QueryPerformanceFrequency(&n); return n.QuadPart; }
void I_OutputMsg(const char *fmt, ...) { char b[8192]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap); logs += b; fputs(b,stdout); }
void I_Error(const char *fmt, ...) { va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); exit(1); }
}
namespace rr_d3d12 {
ID3D11Device *Present_AngleDevice() { return nullptr; }
void Present_SelectPath() {}
void Present_Frame(ID3D12Resource *, D3D12_RESOURCE_STATES, UINT, UINT) {
    // No transition is necessary: submission leaves the source in its supplied state.
    UINT64 fence = Core_Submit("present");
    if (!fence || !Core_Wait(fence, 30000, "headless present")) throw std::runtime_error("present failed");
}
void Present_Immediately() {}
void Present_Shutdown(bool) {}
void Present_GetTrace(INT32 *c,INT32 *s,UINT32 *cf,UINT32 *sf) { if(c)*c=-1; if(s)*s=-1; if(cf)*cf=0; if(sf)*sf=0; }
void Present_LogPerf(double) {}
bool Present_Tracing() { return false; }
void Present_FrameDone() {}
UINT64 Present_SlotBytes() { return 0; }
}
template<class T> T function(const char *name) { auto p=D3D12_GetDriverFunction(name); if(!p) throw std::runtime_error(name); return reinterpret_cast<T>(p); }
static auto clear = function<void (*)(FBOOLEAN,FBOOLEAN,FRGBAFloat *)>("ClearBuffer");
static auto blend = function<void (*)(FBITFIELD)>("SetBlend");
static auto polygon = function<void (*)(FSurfaceInfo *,FOutVector *,FUINT,FBITFIELD)>("DrawPolygon");
static auto indexed = function<void (*)(FSurfaceInfo *,FOutVector *,FUINT,FBITFIELD,UINT32 *)>("DrawIndexedTriangles");
static auto readrect = function<void (*)(INT32,INT32,INT32,INT32,INT32,UINT16 *)>("ReadRect");
static auto line = function<void (*)(F2DCoord *,F2DCoord *,RGBA_t)>("Draw2DLine");
static ID3D12InfoQueue *validationQueue=nullptr;
static void validate(ID3D12InfoQueue *queue);
using RGB=std::array<int,3>;
static constexpr int W=64,H=64;
static void reset(RGB c={48,80,112}) { FRGBAFloat f={c[0]/255.f,c[1]/255.f,c[2]/255.f,1}; clear(true,true,&f); }
static FSurfaceInfo surface(RGB c,int alpha=255) { FSurfaceInfo s={}; s.PolyColor.s.red=(UINT8)c[0]; s.PolyColor.s.green=(UINT8)c[1]; s.PolyColor.s.blue=(UINT8)c[2]; s.PolyColor.s.alpha=(UINT8)alpha; return s; }
static std::vector<FOutVector> quad(float z=1) { return {{-.8f*z,-.8f*z,z,0,0},{.8f*z,-.8f*z,z,0,0},{.8f*z,.8f*z,z,0,0},{-.8f*z,.8f*z,z,0,0}}; }
static void draw(RGB c,FBITFIELD flags=0,float z=1,int alpha=255) { auto v=quad(z); auto s=surface(c,alpha); flags |= PF_Modulated|PF_NoTexture; blend(flags); polygon(&s,v.data(),(FUINT)v.size(),flags); }
static std::vector<UINT8> pixels() { std::vector<UINT8> p(W*H*3,0xcd); readrect(0,0,W,H,W*3,reinterpret_cast<UINT16 *>(p.data())); return p; }
static void expect(const char *scene,RGB want,int x=32,int y=32) { auto p=pixels(); validate(validationQueue); if(negative==scene) want[0]=(want[0]+67)%256; for(int c=0;c<3;c++) if(abs(p[(y*W+x)*3+c]-want[c])>1) { fprintf(stderr,"%s: pixel %d,%d channel %d got %d expected %d\n",scene,x,y,c,p[(y*W+x)*3+c],want[c]); negativeRejected = negative==scene; throw std::runtime_error("pixel oracle failed"); } }
static void validate(ID3D12InfoQueue *queue)
{
    bool failed = false;
    static bool clearWarningLogged = false;
    for (UINT64 i = 0; i < queue->GetNumStoredMessages(); i++)
    {
        SIZE_T bytes = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &bytes))) throw std::runtime_error("GetMessage size failed");
        std::vector<UINT8> data(bytes);
        auto m = reinterpret_cast<D3D12_MESSAGE *>(data.data());
        if (FAILED(queue->GetMessage(i, m, &bytes))) throw std::runtime_error("GetMessage failed");
        // #820 only reports an absent/mismatched optimized clear value. The runtime
        // explicitly guarantees the requested color is still cleared; this driver
        // intentionally clears arbitrary colors, which the pixel oracles verify.
        // It is a performance advisory, never a state or lifetime validation error.
        if (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING &&
            m->ID == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE)
        {
            if (!clearWarningLogged) printf("Allowlisted D3D12 clear optimization advisory #%d: %s\n", m->ID, m->pDescription);
            clearWarningLogged = true;
            continue;
        }
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            fprintf(stderr, "D3D12 validation #%d: %s\n", m->ID, m->pDescription);
            failed = true;
        }
    }
    if (failed) throw std::runtime_error("debug warning/error");
    if (Core_Errors()) throw std::runtime_error("production Core_Errors nonzero");
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--negative-control")) negative=argv[2];
    else if(argc!=1) return 2;
    ComPtr<ID3D12Debug> debug; ComPtr<ID3D12Debug1> gbv;
    if(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || FAILED(debug.As(&gbv))) { fputs("D3D12 debug layer/GBV unavailable; install Windows Graphics Tools\n",stderr); return 77; }
    debug->EnableDebugLayer(); gbv->SetEnableGPUBasedValidation(TRUE);
    puts("D3D12 debug layer and GPU-based validation enabled");
    try {
        Core_ForceWarp(true); D3D12_SetRingBytesForTest(1024,512,2048);
        if(!function<boolean (*)()>("Init")() || !D3D12_Surface(W,H)) throw std::runtime_error("Init/Surface failed");
        ComPtr<ID3D12InfoQueue> queue; if(FAILED(Core_Device()->QueryInterface(IID_PPV_ARGS(&queue)))) throw std::runtime_error("InfoQueue unavailable"); validationQueue=queue.Get();
        function<void (*)(INT32,INT32,INT32,INT32,float)>("GClipRect")(0,0,W,H,.1f);
        reset(); expect("clear",{48,80,112});
        reset({0,0,0});
        { auto upper=quad(); upper[0].y=upper[1].y=.1f; auto red=surface({248,32,16}); polygon(&red,upper.data(),4,PF_Modulated|PF_NoTexture|PF_NoDepthTest);
          auto lower=quad(); lower[2].y=lower[3].y=-.1f; auto blue=surface({16,64,248}); polygon(&blue,lower.data(),4,PF_Modulated|PF_NoTexture|PF_NoDepthTest);
          expect("readrect",{248,32,16},32,12); expect("readrect",{16,64,248},32,52);
          std::vector<UINT8> odd(7*5*3,0xcd); readrect(29,49,7,5,21,reinterpret_cast<UINT16 *>(odd.data()));
          for(size_t i=0;i<odd.size();i++) { const int wanted=RGB{248,32,16}[i%3]; if(abs(odd[i]-wanted)>1) throw std::runtime_error("odd-width/subrect RGB24 oracle failed"); }
          UINT16 packed[35]={}; readrect(29,49,7,5,14,packed); const UINT16 wanted565=(UINT16)((248>>3)<<11 | (32>>2)<<5 | (16>>3)); for(auto c:packed) if(c!=wanted565) throw std::runtime_error("RGB565 oracle failed"); }
        RGB src={160,96,64},dst={48,80,112}; const double a=128./255.;
        struct Mode { const char *name; FBITFIELD flags; } modes[]={{"opaque",0},{"translucent",PF_Translucent},{"additive",PF_Additive},{"subtract",PF_Subtractive},{"reverse",PF_ReverseSubtract},{"multiply",PF_Multiplicative},{"environment",PF_Environment},{"masked",PF_Masked},{"invert",PF_Invert},{"fog",PF_Fog}};
        for(auto m:modes) { reset(dst); draw(src,m.flags|PF_NoDepthTest,1,128); RGB result={}; for(int c=0;c<3;c++) { double s=src[c],d=dst[c],v=s; switch(m.flags) { case PF_Translucent:v=s*a+d*(1-a);break; case PF_Additive:v=s*a+d;break; case PF_Subtractive:v=s*a-d;break; case PF_ReverseSubtract:v=d-s*a;break; case PF_Multiplicative:v=s*d/255;break; case PF_Environment:v=s+d*(1-a);break; case PF_Masked:v=s*a;break; case PF_Invert:v=s*(1-d/255);break; case PF_Fog:v=s*a+d*s/255;break; } result[c]=(int)std::lround(std::max(0.,std::min(255.,v))); } expect(m.name,result); D3D12_FinishUpdate(0); }
        reset(); draw({200,40,20},PF_Occlude,.5f); draw({20,200,40},PF_Occlude,1.5f); expect("depth",{200,40,20});
        draw({20,40,200},PF_NoDepthTest,1.5f); expect("nodepth",{20,40,200});
        reset(); draw({200,40,20},0,.5f); draw({20,200,40},PF_Occlude,1.5f); expect("noocclude",{20,200,40});
        reset(); auto v=quad(); auto s=surface({24,180,220}); UINT32 ix[]={0,1,2,0,2,3}; indexed(&s,v.data(),6,PF_Modulated|PF_NoTexture|PF_NoDepthTest,ix); expect("indexed",{24,180,220});
        reset(); blend(PF_NoDepthTest|PF_NoTexture); F2DCoord p1={-.75f,0},p2={.75f,0}; auto ls=surface({240,160,80}); line(&p1,&p2,ls.PolyColor);
        // A one-pixel line lies on a raster boundary; inspect its known interior coverage, excluding endpoints.
        { auto p=pixels(); validate(queue.Get()); bool found=false; for(int y=30;y<=33;y++) { int o=(y*W+32)*3; if(abs(p[o]-240)<=1 && abs(p[o+1]-160)<=1 && abs(p[o+2]-80)<=1) found=true; } if(negative=="line") found=false; if(!found) { negativeRejected=negative=="line"; throw std::runtime_error("line oracle failed"); } }
        for(int frame=0;frame<4;frame++) { reset(); for(int i=0;i<1000;i++) draw({(i&1)?80:120,40,160},PF_NoDepthTest); expect("rings",{80,40,160}); D3D12_FinishUpdate(0); }
        reset(); std::vector<FOutVector> huge(400,quad()[0]); huge[0]=quad()[0]; huge[1]=quad()[1]; huge[2]=quad()[2]; huge[3]=quad()[3]; auto hs=surface({32,220,96}); polygon(&hs,huge.data(),(FUINT)huge.size(),PF_Modulated|PF_NoTexture|PF_NoDepthTest); expect("growth",{32,220,96});
        D3D12_FinishUpdate(0); D3D12_LogPerf(1);
        unsigned vw=0,iw=0,cw=0,st=0,grows=0; auto pos=logs.find("; wraps "); if(pos==std::string::npos || sscanf(logs.c_str()+pos,"; wraps %u/%u/%u; stalls %u",&vw,&iw,&cw,&st)!=4) throw std::runtime_error("missing ring perf counters"); auto gp=logs.find("; grows ",pos); if(gp==std::string::npos || sscanf(logs.c_str()+gp,"; grows %u",&grows)!=1 || !grows || !(vw+iw+cw) || !st) throw std::runtime_error("stress did not wrap/stall and grow rings");
        unsigned ringSubmissions=0; auto rp=logs.find("mid-frame: ring "); if(rp==std::string::npos || sscanf(logs.c_str()+rp,"mid-frame: ring %u",&ringSubmissions)!=1 || !ringSubmissions) throw std::runtime_error("stress had no ring submissions");
        Prewarm_Stop(); if(!Core_WaitIdle(30000,"validation")) throw std::runtime_error("idle failed"); validate(queue.Get());
        D3D12_Shutdown(); validate(queue.Get());
        const bool closed = Core_Device() == nullptr;
        const bool expectedClosed = negative != "shutdown";
        if (closed != expectedClosed) {
            negativeRejected = negative == "shutdown";
            throw std::runtime_error("shutdown device lifetime oracle failed");
        }
        if(!negative.empty()) throw std::runtime_error("unknown or unexercised negative control");
        puts("Production D3D12 WARP driver scenes passed (RGB tolerance 1, GBV clean)"); return 0;
    } catch(const std::exception &e) { fprintf(stderr,"FAIL: %s\n",e.what()); D3D12_Shutdown(); if(negativeRejected) fprintf(stderr,"NEGATIVE_ORACLE_REJECTED: %s\n",negative.c_str()); return 1; }
}
