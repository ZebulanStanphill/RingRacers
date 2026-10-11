// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
// Runs the production driver and GPU code; only engine services and ANGLE presentation are stubbed.
#include "hardware/r_d3d12/d3d12_core.h"
#include "hardware/r_d3d12/d3d12_present.h"
#include "hardware/r_d3d12/d3d12_prewarm.h"
#include "doomdef.h"
#include "command.h"
#include "i_system.h"
#include "p_tick.h"
#include "r_fps.h"
#include "doomstat.h"
#include "f_finale.h"
#include "w_wad.h"
#include "hardware/hw_drv.h"
#include "hardware/hw_main.h"
#include "hardware/hw_data.h"
#include "hardware/hw_model.h"
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
#include "t_fsin.c"
#undef min
#undef max
#undef clamp
using namespace rr_d3d12;
static_assert((PF_Invert & PF_Occlude) == 0, "Inversion must not alias depth writes");
static std::string logs, negative;
static bool negativeRejected=false;
static std::vector<UINT8> lumpBytes; // what the stubbed wipe mask lump holds
extern "C" {
UINT8 g_wipemode=0, g_wipetype=0, g_wipeframe=0;
boolean g_wipereverse=0, g_wipeencorewiggle=0;
// The tests' wipe modes: 1 to black, 2 to white, 3 to invert, otherwise a crossfade
boolean F_WipeIsToBlack(UINT8 m) { return m==1; }
boolean F_WipeIsToWhite(UINT8 m) { return m==2; }
boolean F_WipeIsToInvert(UINT8 m) { return m==3; }
tic_t leveltime=0;
postimg_t postimgtype[MAXSPLITSCREENPLAYERS]={};
enum viewcontext_e viewcontext=VIEWCONTEXT_PLAYER1;
// FADE0000 and FADE0001 exist; any other mask is missing, which leaves the two-pass wipe
lumpnum_t W_CheckNumForName(const char *n) { return (!strcmp(n,"FADE0000")||!strcmp(n,"FADE0001")) ? 1 : LUMPERROR; }
size_t W_LumpLength(lumpnum_t) { return lumpBytes.size(); }
void W_ReadLump(lumpnum_t, void *d) { memcpy(d,lumpBytes.data(),lumpBytes.size()); }
consvar_t cv_gld3d12{}, cv_vidwait{}, cv_reducevfx{};
CV_PossibleValue_t glanisotropicmode_cons_t[] = {{0, "Off"}, {16, "16"}, {0, nullptr}};
INT32 R_AdjustLightLevel(INT32 light) { return light; }
void HWR_Startup() {}
void CV_StealthSet(consvar_t *, const char *) {}
void M_SaveConfig(const char *) {}
UINT32 HWR_UWPTakeLazyModelLoads(void) { return 0; }
precise_t I_GetPreciseTime() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
UINT64 I_GetPrecisePrecision() { LARGE_INTEGER n; QueryPerformanceFrequency(&n); return n.QuadPart; }
void I_OutputMsg(const char *fmt, ...) { char b[8192]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap); logs += b; fputs(b,stdout); }
RGBA_t *pMasterPalette = nullptr;
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
// Palette rendering's last step, read like expect reads the screen. scene names its own negative control.
static void expectPost(const char *scene,RGB want,int x=32,int y=32) { std::vector<UINT8> p(W*H*3,0xcd); D3D12_ReadPaletteRectForTest(0,0,W,H,W*3,reinterpret_cast<UINT16 *>(p.data())); validate(validationQueue); if(negative==scene) want[0]=(want[0]+67)%256; for(int c=0;c<3;c++) if(abs(p[(y*W+x)*3+c]-want[c])>1) { fprintf(stderr,"%s: palette pixel %d,%d channel %d got %d expected %d\n",scene,x,y,c,p[(y*W+x)*3+c],want[c]); negativeRejected = negative==scene; throw std::runtime_error("palette pixel oracle failed"); } }
// Compares a pixel of an already read image; scene names its own negative control.
static void check(const char *scene,const std::vector<UINT8> &p,int x,int y,RGB want,int width=W) { if(negative==scene) want[0]=(want[0]+67)%256; for(int c=0;c<3;c++) if(abs(p[(y*width+x)*3+c]-want[c])>1) { fprintf(stderr,"%s: pixel %d,%d channel %d got %d expected %d (got %d,%d,%d expected %d,%d,%d)\n",scene,x,y,c,p[(y*width+x)*3+c],want[c],p[(y*width+x)*3],p[(y*width+x)*3+1],p[(y*width+x)*3+2],want[0],want[1],want[2]); negativeRejected = negative==scene; throw std::runtime_error("pixel oracle failed"); } }
static void fillRect(float x0,float x1,float y0,float y1,RGB c) { std::vector<FOutVector> v={{x0,y0,1,0,0},{x1,y0,1,0,0},{x1,y1,1,0,0},{x0,y1,1,0,0}}; auto s=surface(c); const FBITFIELD f=PF_Modulated|PF_NoTexture|PF_NoDepthTest; blend(f); polygon(&s,v.data(),4,f); }
static void quadrants(RGB tl,RGB tr,RGB bl,RGB br) { reset({0,0,0}); fillRect(-1,0,0,1,tl); fillRect(0,1,0,1,tr); fillRect(-1,0,-1,0,bl); fillRect(0,1,-1,0,br); }
// The screen's color at (x, y from the top) at the current size
static RGB probe(int x,int y,int height) { UINT8 p[3]={0xcd,0xcd,0xcd}; readrect(x,height-1-y,1,1,3,reinterpret_cast<UINT16 *>(p)); return {p[0],p[1],p[2]}; }
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
        auto texture = function<void (*)(GLMipmap_t *)>("SetTexture");
        auto updateTexture = function<void (*)(GLMipmap_t *)>("UpdateTexture");
        auto deleteTexture = function<void (*)(GLMipmap_t *)>("DeleteTexture");
        auto special = function<void (*)(hwdspecialstate_t,INT32)>("SetSpecialState");
        special(HWD_SET_TEXTUREFILTERMODE,HWD_SET_TEXTUREFILTER_POINTSAMPLED);
        // Screen copies are bound to unit 0 without telling SetTexture's bookkeeping (as in the Direct3D 11 driver), so unbind it
        // through a real texture
        auto unbindTextures = [&]() { std::array<UINT8,4> white={255,255,255,255}; GLMipmap_t one={}; one.format=GL_TEXFMT_RGBA; one.width=one.height=1; one.data=white.data(); texture(&one); texture(nullptr); deleteTexture(&one); };
        auto textured = [&](float left=-.8f,float right=.8f,float size=.8f) {
            auto q=quad(); q[0].x=q[3].x=left; q[1].x=q[2].x=right;
            q[0].y=q[1].y=-size; q[2].y=q[3].y=size;
            q[0].s=q[3].s=0; q[1].s=q[2].s=1;
            q[0].t=q[1].t=0; q[2].t=q[3].t=1;
            auto white=surface({255,255,255}); const FBITFIELD flags=PF_Modulated|PF_NoDepthTest;
            blend(flags); polygon(&white,q.data(),4,flags);
        };
        std::array<UINT8,16> texels={240,32,16,255, 16,224,48,255, 32,48,240,255, 224,192,16,255};
        GLMipmap_t tex={}; tex.format=GL_TEXFMT_RGBA; tex.width=tex.height=2; tex.data=texels.data();
        reset(); texture(&tex); textured();
        // Texture coordinates increase from bottom to top in the driver's default projection.
        expect("texture",{240,32,16},20,44); expect("texture",{16,224,48},44,44);
        expect("texture",{32,48,240},20,20); expect("texture",{224,192,16},44,20);
        std::array<UINT8,16> solid={200,40,16,255,200,40,16,255,200,40,16,255,200,40,16,255};
        tex.data=solid.data(); updateTexture(&tex); reset(); texture(&tex); textured(-.8f,-.1f);
        for(int i=0;i<4;i++) { solid[i*4]=16; solid[i*4+1]=200; solid[i*4+2]=80; }
        // No ReadRect/present between draws: an in-place upload must retain the first draw's version.
        updateTexture(&tex); texture(&tex); textured(.1f,.8f);
        expect("texture-update",{200,40,16},18,32); expect("texture-update",{16,200,80},46,32);
        deleteTexture(&tex); D3D12_FinishUpdate(0);
        for(int frame=0;frame<96;frame++) {
            const RGB c={(frame&1)?208:32,64,(frame&1)?24:216};
            for(int i=0;i<4;i++) { solid[i*4]=(UINT8)c[0]; solid[i*4+1]=(UINT8)c[1]; solid[i*4+2]=(UINT8)c[2]; }
            reset(); texture(&tex); textured(); deleteTexture(&tex);
            // Delete while recorded work still references the placed resource and descriptor.
            expect("texture-reuse",c); D3D12_FinishUpdate(0);
        }
        UINT64 placed=0,blockReuses=0,srvReuses=0;
        D3D12_GetReuseCountsForTest(&placed,&blockReuses,&srvReuses);
        if(!placed || !blockReuses || !srvReuses) throw std::runtime_error("placed block/SRV reuse was not exercised");
        // At LOD 3 the two bright columns in each eight average to 72.
        // Base-level bilinear sampling hits the two bright columns (240), so missing mips fail.
        std::vector<UINT8> checker(64*64*4);
        for(int y=0;y<64;y++) for(int x=0;x<64;x++) { int o=(y*64+x)*4; UINT8 c=(x%8==3 || x%8==4)?240:16; checker[o]=checker[o+1]=checker[o+2]=c; checker[o+3]=255; }
        tex.width=tex.height=64; tex.data=checker.data();
        special(HWD_SET_TEXTUREFILTERMODE,HWD_SET_TEXTUREFILTER_TRILINEAR);
        reset(); texture(&tex); textured(-.125f,.125f,.125f); expect("mips",{72,72,72});
        deleteTexture(&tex); special(HWD_SET_TEXTUREFILTERMODE,HWD_SET_TEXTUREFILTER_POINTSAMPLED);
        // Palette rendering. The lookup table maps a color to index 7 only when red is below 32 and green and blue aren't,
        // otherwise 19, so a lookup upload that swaps axes or misplaces rows or slices picks the wrong index.
        {
            auto setLookup=function<void (*)(UINT8 *)>("SetPaletteLookup");
            auto setScreenPalette=function<void (*)(RGBA_t *)>("SetScreenPalette");
            auto createLightTable=function<UINT32 (*)(RGBA_t *)>("CreateLightTable");
            auto updateLightTable=function<void (*)(UINT32,RGBA_t *)>("UpdateLightTable");
            auto clearLightTables=function<void (*)()>("ClearLightTables");
            auto setShader=function<void (*)(int)>("SetShader");
            constexpr int lutSize=HWR_PALETTE_LUT_SIZE;
            std::vector<UINT8> lut(lutSize*lutSize*lutSize);
            for(int b=0;b<lutSize;b++) for(int g=0;g<lutSize;g++) for(int r=0;r<lutSize;r++) lut[(b*lutSize+g)*lutSize+r]=(r<lutSize/2 && g>=lutSize/2 && b>=lutSize/2)?7:19;
            auto rgba=[](RGB c) { RGBA_t v={}; v.s.red=(UINT8)c[0]; v.s.green=(UINT8)c[1]; v.s.blue=(UINT8)c[2]; v.s.alpha=255; return v; };
            const RGB oldColor={180,60,80}, newColor={32,180,200}; // lit colors: lookup index 19, then 7
            const RGB index7={40,216,104}, index19={232,152,24};
            std::array<RGBA_t,256> screenPalette,masterPalette;
            screenPalette.fill(rgba({248,8,240})); screenPalette[7]=rgba(index7); screenPalette[19]=rgba(index19);
            masterPalette.fill(rgba({8,8,8}));
            // Every other level and index is magenta-ish, so a wrong light level or index fails
            std::vector<RGBA_t> lightTable(256*33,rgba({252,12,244}));
            lightTable[7]=rgba(oldColor);
            pMasterPalette=masterPalette.data();
            setLookup(lut.data()); setScreenPalette(screenPalette.data());
            special(HWD_SET_SHADERS,HWD_SHADEROPTION_ON);
            special(HWD_SET_PALETTE_RENDERING,1); setShader(SHADER_FLOOR);
            std::array<UINT8,4> paletteTexel={24,180,220,255}; // index 7
            GLMipmap_t paletteTexture={}; paletteTexture.format=GL_TEXFMT_RGBA; paletteTexture.width=paletteTexture.height=1; paletteTexture.data=paletteTexel.data();
            texture(&paletteTexture);
            auto paletteDraw=[&](UINT32 id,float left=-.8f,float right=.8f) {
                texture(&paletteTexture);
                auto q=quad(); q[0].x=q[3].x=left; q[1].x=q[2].x=right;
                auto white=surface({255,255,255}); white.LightTableId=id; white.LightInfo.light_level=255;
                const FBITFIELD flags=PF_Modulated|PF_ColorMapped|PF_NoDepthTest;
                blend(flags); polygon(&white,q.data(),4,flags);
            };
            UINT32 lightId=createLightTable(lightTable.data());
            if(!lightId) throw std::runtime_error("CreateLightTable failed");
            reset(); paletteDraw(lightId);
            // The screen holds the light table's color; the finished frame holds the screen palette's color for its index.
            expect("palette",oldColor); expectPost("palette-post",index19);
            // Presenting again repeats the pass on the same screen
            D3D12_FinishUpdate(0);
            // b) One frame, no readback between the draws: the first keeps the old table, the second gets the new one.
            reset(); paletteDraw(lightId,-.8f,-.1f);
            lightTable[7]=rgba(newColor); updateLightTable(lightId,lightTable.data());
            paletteDraw(lightId,.1f,.8f);
            expect("light-table-update",oldColor,18,32); expect("light-table-update",newColor,46,32);
            expectPost("light-table-update-post",index19,18,32); expectPost("light-table-update-post",index7,46,32);
            D3D12_FinishUpdate(0);
            // Palette fog chooses the captured scene index and Software's integer distance row.
            for(bool plane:{false,true}) {
                const float distance=128.125f; const int lightnum=12;
                int attenuation=plane?(160/((int)(distance/16)+1))/2:std::min((int)(5120/distance),47)/2;
                int row=std::clamp((31-lightnum)*2-attenuation,0,31);
                lightTable.assign(256*33,rgba({252,12,244}));
                const RGB remapped={20+row*5,210-row*3,48+row*2};
                lightTable[row*256+7]=rgba(remapped); updateLightTable(lightId,lightTable.data());
                // Draw scenery with shaders off so its RGB is exactly the LUT's index 7 color.
                function<void (*)()>("UnSetShader")(); reset({24,180,220}); setShader(SHADER_FOG);
                auto fog=surface({248,4,252},128); fog.LightTableId=lightId;
                fog.LightInfo.light_level=lightnum*8; fog.LightInfo.fade_end=31;
                auto q=quad(distance);
                polygon(&fog,q.data(),4,PF_Modulated|PF_ColorMapped|PF_Fog|PF_NoTexture|PF_NoDepthTest|(plane?PF_FogPlane:0));
                expect("fog-palette",remapped);
                expect("fog-palette",{24,180,220},2,2);
                D3D12_FinishUpdate(0);
            }
            // Restore the table and shader for the lifetime scenes below.
            lightTable.assign(256*33,rgba({252,12,244})); texture(&paletteTexture); setShader(SHADER_FLOOR);
            // c) Clear and remake the table every frame, so its views and resources are retired while recorded work uses them,
            // and descriptors come back after the fence.
            UINT64 placedBefore=0,blocksBefore=0,srvsBefore=0; D3D12_GetReuseCountsForTest(&placedBefore,&blocksBefore,&srvsBefore);
            for(int frame=0;frame<96;frame++) {
                const RGB lit=(frame&1)?oldColor:newColor;
                lightTable[7]=rgba(lit); clearLightTables();
                lightId=createLightTable(lightTable.data());
                if(!lightId) throw std::runtime_error("recreating a light table failed");
                reset(); paletteDraw(lightId); clearLightTables();
                expect("light-table-clear",lit); expectPost("light-table-clear",(frame&1)?index19:index7);
                D3D12_FinishUpdate(0);
            }
            UINT64 placedAfter=0,blocksAfter=0,srvsAfter=0; D3D12_GetReuseCountsForTest(&placedAfter,&blocksAfter,&srvsAfter);
            if(srvsAfter<=srvsBefore) throw std::runtime_error("light table SRV reuse was not exercised");
            // d) The screen palette changes between frames and recolors the same screen
            lightTable[7]=rgba(oldColor); lightId=createLightTable(lightTable.data());
            for(int frame=0;frame<4;frame++) {
                const RGB flash=(frame&1)?RGB{16,196,232}:index19;
                screenPalette[19]=rgba(flash); setScreenPalette(screenPalette.data());
                reset(); paletteDraw(lightId);
                expect("screen-palette",oldColor); expectPost("screen-palette",flash);
                D3D12_FinishUpdate(0);
            }
            // With palette rendering off, there is no last step to read
            special(HWD_SET_PALETTE_RENDERING,0);
            { std::vector<UINT8> p(W*H*3,0xcd); D3D12_ReadPaletteRectForTest(0,0,W,H,W*3,reinterpret_cast<UINT16 *>(p.data()));
              bool untouched=true; for(auto c:p) untouched=untouched && c==0xcd; if(negative=="palette-off") untouched=false;
              if(!untouched) { negativeRejected=negative=="palette-off"; throw std::runtime_error("palette step ran with palette rendering off"); } }
            D3D12_FinishUpdate(0);
            pMasterPalette=nullptr; setLookup(lut.data()); // without a master palette, it goes
            clearLightTables(); deleteTexture(&paletteTexture);
            function<void (*)()>("UnSetShader")(); special(HWD_SET_SHADERS,HWD_SHADEROPTION_OFF);
            validate(validationQueue);
        }
        // Explicit fog boundaries composite the captured scenery, irrespective of polygon color.
        {
            auto shader=function<void (*)(int)>("SetShader");
            special(HWD_SET_SHADERS,HWD_SHADEROPTION_ON); function<void (*)()>("UnSetShader")();
            const RGB colors[]={{200,40,16},{16,64,224},{32,200,64},{232,216,24}};
            quadrants(colors[0],colors[1],colors[2],colors[3]); shader(SHADER_FOG);
            auto fog=surface({252,4,248},128); fog.LightInfo.light_level=255; fog.LightInfo.fade_end=31;
            fog.TintColor.s.red=51; fog.TintColor.s.green=76; fog.TintColor.s.blue=128; fog.TintColor.s.alpha=10;
            auto q=quad(128.125f);
            polygon(&fog,q.data(),4,PF_Modulated|PF_ColorMapped|PF_Fog|PF_NoTexture|PF_NoDepthTest);
            auto image=pixels(); validate(validationQueue);
            for(int i=0;i<4;i++) {
                double brightness=std::sqrt(double(colors[i][0]*colors[i][0]+colors[i][1]*colors[i][1]+colors[i][2]*colors[i][2]));
                double strength=std::sqrt(9.*10/255); RGB want={}; const int tint[]={51,76,128};
                for(int c=0;c<3;c++) want[c]=(int)std::lround(std::clamp(brightness*tint[c]/255*strength+colors[i][c]*(1-strength),0.,255.));
                check("fog-remap",image,(i&1)?44:20,(i&2)?44:20,want);
            }
            function<void (*)()>("UnSetShader")(); special(HWD_SET_SHADERS,HWD_SHADEROPTION_OFF); D3D12_FinishUpdate(0);
        }
        // Water surface UV distortion and scene refraction use separate D3D11 equations.
        {
            auto shader=function<void (*)(int)>("SetShader");
            auto shaderInfo=function<void (*)(hwdshaderinfo_t,INT32)>("SetShaderInfo");
            special(HWD_SET_SHADERS,HWD_SHADEROPTION_ON); shader(SHADER_WATER);
            std::vector<UINT8> stripe(64*4);
            for(int y=0;y<64;y++) { stripe[y*4]=UINT8(y*3); stripe[y*4+1]=UINT8(240-y*3); stripe[y*4+2]=64; stripe[y*4+3]=255; }
            GLMipmap_t water={}; water.format=GL_TEXFMT_RGBA; water.width=1; water.height=64; water.data=stripe.data();
            texture(&water);
            auto ws=surface({255,255,255}); ws.LightInfo.light_level=255; ws.LightInfo.fade_end=31;
            const float z=128.125f; auto q=quad(z); q[0].t=q[1].t=0; q[2].t=q[3].t=1;
            for(int tick:{0,6,41,128}) {
                shaderInfo(HWD_SHADERINFO_LEVELTIME,tick); reset(); texture(&water);
                polygon(&ws,q.data(),4,PF_Modulated|PF_ColorMapped|PF_Ripple|PF_NoDepthTest);
                auto image=pixels(); validate(validationQueue);
                for(int y:{20,32,44}) {
                    // Production perspective remaps clip Z to D3D; PSWater then divides fragz by two.
                    float fragz=(z-.1f)*32768.f/(32768.f-.1f);
                    float v=(57.6f-(y+.5f))/51.2f-std::cos(-3.14159f*(fragz/2*.025f)+tick/35.f*2)*.025f;
                    int row=((int)std::floor(v*64)%64+64)%64;
                    check("water-surface",image,32,y,{row*3,240-row*3,64});
                }
            }
            deleteTexture(&water);
            std::array<UINT8,4> transparent={0,0,0,1}; water.height=1; water.data=transparent.data(); texture(&water);
            for(float distance:{32.125f,128.125f,512.125f}) for(int tick:{0,6,41,128}) {
                function<void (*)()>("UnSetShader")();
                reset({0,0,0});
                for(int y=0;y<64;y++) fillRect(-1,1,1-(y+1)/32.f,1-y/32.f,{y*3,240-y*3,64});
                texture(&water); shader(SHADER_WATER); shaderInfo(HWD_SHADERINFO_LEVELTIME,tick); auto wq=quad(distance);
                polygon(&ws,wq.data(),4,PF_Modulated|PF_ColorMapped|PF_Ripple|PF_Translucent|PF_NoDepthTest);
                auto image=pixels(); validate(validationQueue);
                int angle=(tick*140+(int)std::floor(distance*128))&8191;
                int shift=(int)std::floor(double(finesine[angle])/(4096+std::floor(distance*32)));
                for(int y:{20,32,44}) { int row=std::clamp(y+shift,0,63); check("water-refraction",image,32,y,{(int)std::lround(row*3*254./255),(int)std::lround((240-row*3)*254./255),64}); }
                D3D12_FinishUpdate(0);
            }
            // Nested coplanar faces reuse the run's first capture; a second run gets a fresh one.
            // Tiny constant/geometry rings may submit between these draws, exercising capture lifetime.
            reset({80,120,160}); shaderInfo(HWD_SHADERINFO_LEVELTIME,6);
            UINT64 fogBefore=0,copiesBefore=0,tripsBefore=0;
            D3D12_GetCaptureCountsForTest(&fogBefore,&copiesBefore,&tripsBefore);
            for(int run=0;run<2;run++) {
                special(HWD_SET_WATER_SURFACE,1);
                for(int face=0;face<8;face++) {
                    if(face) special(HWD_SET_WATER_SURFACE,2);
                    auto nested=quad(128.125f); float scale=1-face*.025f;
                    for(auto &vertex:nested) { vertex.x*=scale; vertex.y*=scale; }
                    polygon(&ws,nested.data(),4,PF_Modulated|PF_ColorMapped|PF_Ripple|PF_Translucent|PF_NoDepthTest);
                }
            }
            special(HWD_SET_WATER_SURFACE,0);
            UINT64 fogAfter=0,copiesAfter=0,tripsAfter=0;
            D3D12_GetCaptureCountsForTest(&fogAfter,&copiesAfter,&tripsAfter);
            bool grouped=copiesAfter-copiesBefore==2 && tripsAfter-tripsBefore==2;
            if(negative=="water-group-counts") grouped=false;
            if(!grouped) { negativeRejected=negative=="water-group-counts"; throw std::runtime_error("grouped water copy/roundtrip oracle failed"); }
            expect("water-group",{80,120,158}); D3D12_FinishUpdate(0);
            auto fog=surface({248,4,252}); fog.LightInfo.light_level=255; fog.LightInfo.fade_end=31;
            auto captureQuad=quad(128.125f);
            const FBITFIELD waterFlags=PF_Modulated|PF_ColorMapped|PF_Ripple|PF_Translucent|PF_NoDepthTest;
            const FBITFIELD fogFlags=PF_Modulated|PF_ColorMapped|PF_Fog|PF_NoTexture|PF_NoDepthTest;
            auto waterDraw=[&](UINT count=4) { texture(&water); shader(SHADER_WATER); polygon(&ws,captureQuad.data(),count,waterFlags); };
            auto fogDraw=[&](UINT count=4) { shader(SHADER_FOG); polygon(&fog,captureQuad.data(),count,fogFlags); };
            auto counts=[&]() { std::array<UINT64,3> result={}; D3D12_GetCaptureCountsForTest(&result[0],&result[1],&result[2]); return result; };
            auto countDelta=[&](const char *scene,std::array<UINT64,3> before,UINT64 fogCount,UINT64 waterCount) {
                auto after=counts(); bool correct=after[0]-before[0]==fogCount && after[1]-before[1]==waterCount && after[2]-before[2]==waterCount;
                if(negative==scene) correct=false;
                if(!correct) { negativeRejected=negative==scene; throw std::runtime_error("capture copy/roundtrip delta failed"); }
            };
            bool mixedSameList=false;
            for(int attempt=0;attempt<4 && !mixedSameList;attempt++) {
                reset({80,120,160}); auto mixedBefore=counts();
                waterDraw(); UINT64 pending=Core_PendingFence(); fogDraw();
                mixedSameList=Core_PendingFence()==pending;
                countDelta("capture-mixed",mixedBefore,1,1); expect("capture-mixed",{80,120,159});
                // Shift the constant-ring head after a split pair; otherwise its next wrap can
                // repeat at the same point in every attempt. The next reset discards this draw.
                if(!mixedSameList) { function<void (*)()>("UnSetShader")(); draw({24,180,220},PF_NoDepthTest); }
            }
            if(!mixedSameList) throw std::runtime_error("mixed captures never shared a command list");
            D3D12_FinishUpdate(0);

            // Flush while both captured textures are referenced by recorded draws. Continuing the old
            // grouped run must capture afresh, rather than retaining its retired snapshot or ready flag.
            reset({80,120,160}); special(HWD_SET_WATER_SURFACE,1); waterDraw(); fogDraw();
            function<void (*)()>("FlushScreenTextures")();
            auto recreateBefore=counts(); reset({40,80,120}); special(HWD_SET_WATER_SURFACE,2);
            waterDraw(); fogDraw(); countDelta("capture-recreate",recreateBefore,1,1);
            expect("capture-recreate",{40,80,120}); D3D12_FinishUpdate(0);
            // Actual size changes replace both capture allocations. Keep a grouped run ready across
            // each Surface call and verify that state 2 still reads the new target's known color.
            auto clip=function<void (*)(INT32,INT32,INT32,INT32,float)>("GClipRect");
            for(int size:{128,64}) {
                special(HWD_SET_WATER_SURFACE,1); waterDraw(); fogDraw();
                if(!D3D12_Surface(size,size)) throw std::runtime_error("capture resize failed");
                clip(0,0,size,size,.1f); reset({40,80,120}); special(HWD_SET_WATER_SURFACE,2);
                recreateBefore=counts(); waterDraw(); fogDraw(); countDelta("capture-recreate",recreateBefore,1,1);
                RGB actual=probe(size/2,size/2,size),want={40,80,120};
                if(negative=="capture-recreate") want[0]+=67;
                for(int c=0;c<3;c++) if(abs(actual[c]-want[c])>1) { negativeRejected=negative=="capture-recreate"; throw std::runtime_error("capture pixel after resize failed"); }
                validate(validationQueue); D3D12_FinishUpdate(0);
            }
            special(HWD_SET_WATER_SURFACE,0);

            // PreparePolygon captures these two diagonal vertices, but a two-vertex fan never
            // draws. Repeated water/fog pairs exercise coalescing of still-pending transitions.
            reset({80,120,160}); auto skippedBefore=counts(); bool sameList=false;
            auto fullQuad=captureQuad; captureQuad[1]=captureQuad[2];
            for(int pair=0;pair<8;pair++) {
                UINT64 pending=Core_PendingFence(); waterDraw(2); fogDraw(2);
                if(Core_PendingFence()==pending) sameList=true;
            }
            captureQuad=fullQuad; waterDraw(); fogDraw();
            bool skippedCorrect=sameList;
            if(negative=="capture-skipped") skippedCorrect=false;
            if(!skippedCorrect) { negativeRejected=negative=="capture-skipped"; throw std::runtime_error("skipped captures did not share a pending command list"); }
            countDelta("capture-skipped",skippedBefore,9,9); expect("capture-skipped",{80,120,159}); D3D12_FinishUpdate(0);
            deleteTexture(&water); function<void (*)()>("UnSetShader")(); special(HWD_SET_SHADERS,HWD_SHADEROPTION_OFF);
        }
        const RGB tl={200,40,16}, tr={16,64,224}, bl={32,200,64}, br={232,216,24}; // quadrants of the screen the copies hold
        // Screen textures: copies of the screen, drawn back as the intermission background and the final screen
        {
            auto makeScreen=function<void (*)()>("MakeScreenTexture");
            auto makeFinal=function<void (*)()>("MakeScreenFinalTexture");
            auto drawIntermission=function<void (*)()>("DrawIntermissionBG");
            auto drawFinal=function<void (*)(int,int)>("DrawScreenFinalTexture");
            auto flushScreens=function<void (*)()>("FlushScreenTextures");
            auto clip=function<void (*)(INT32,INT32,INT32,INT32,float)>("GClipRect");
            // a) The copy holds the quadrants, even after the screen is cleared to something else
            quadrants(tl,tr,bl,br); makeScreen(); reset({96,96,96});
            blend(PF_NoDepthTest); drawIntermission();
            { auto p=pixels(); validate(validationQueue);
              check("screen-copy",p,16,16,tl); check("screen-copy",p,48,16,tr); check("screen-copy",p,16,48,bl); check("screen-copy",p,48,48,br); }
            D3D12_FinishUpdate(0);
            // b) The final screen, drawn into a viewport twice as wide as it is tall: a quarter of the width has black bars on each side
            quadrants(tl,tr,bl,br); makeFinal(); reset({96,96,96});
            blend(PF_NoDepthTest); drawFinal(64,32);
            { auto p=pixels(); validate(validationQueue);
              check("final-screen",p,24,40,tl); check("final-screen",p,40,40,tr); check("final-screen",p,24,56,bl); check("final-screen",p,40,56,br);
              check("final-screen",p,4,40,{0,0,0}); check("final-screen",p,60,56,{0,0,0}); check("final-screen",p,32,8,{0,0,0}); }
            clip(0,0,W,H,.1f);
            D3D12_FinishUpdate(0);
            // c) Flushed, the screen textures are remade, and the new copy shows
            flushScreens();
            quadrants(br,bl,tr,tl); makeScreen(); reset({96,96,96});
            blend(PF_NoDepthTest); drawIntermission();
            { auto p=pixels(); validate(validationQueue);
              check("screen-flush",p,16,16,br); check("screen-flush",p,48,16,bl); check("screen-flush",p,16,48,tr); check("screen-flush",p,48,48,tl); }
            // The copies are retired while recorded work still draws them, and their descriptors come back after the fence
            UINT64 placedBefore=0,blocksBefore=0,srvsBefore=0; D3D12_GetReuseCountsForTest(&placedBefore,&blocksBefore,&srvsBefore);
            for(int frame=0;frame<64;frame++) {
                const RGB a=(frame&1)?tl:br, b=(frame&1)?tr:bl;
                quadrants(a,b,b,a); makeScreen(); reset({96,96,96});
                blend(PF_NoDepthTest); drawIntermission(); flushScreens();
                auto p=pixels(); validate(validationQueue);
                check("screen-flush",p,16,16,a); check("screen-flush",p,48,16,b); check("screen-flush",p,16,48,b); check("screen-flush",p,48,48,a);
                D3D12_FinishUpdate(0);
            }
            UINT64 placedAfter=0,blocksAfter=0,srvsAfter=0; D3D12_GetReuseCountsForTest(&placedAfter,&blocksAfter,&srvsAfter);
            if(srvsAfter<=srvsBefore) throw std::runtime_error("screen texture SRV reuse was not exercised");
        }
        // Wipes. The mask lump is 80x50, its first row at the bottom of the screen; each pixel's byte is 31 - 31 * the share of the end screen.
        int fullWipes=0, twoPassWipes=0;
        {
            auto startWipe=function<void (*)()>("StartScreenWipe");
            auto endWipe=function<void (*)()>("EndScreenWipe");
            auto doWipe=function<void (*)()>("DoScreenWipe");
            const RGB S={150,96,64}, E={16,64,224};
            auto sat=[](double v) { return std::max(0.,std::min(1.,v)); };
            // The shader's formulas for one pixel, in the shader's modes (1 invert, 2 black, 3 white, otherwise a crossfade)
            auto wiped=[&](int mode,RGB s,RGB e,int maskByte,bool reverse) {
                const double fade=1.0-(reverse?32.0-maskByte:(double)maskByte)/31.0;
                double st[3],en[3],o[3];
                for(int c=0;c<3;c++) { st[c]=s[c]/255.; en[c]=e[c]/255.; }
                if(mode==1) for(int c=0;c<3;c++) o[c]=st[c]+(1-st[c]-st[c])*fade;
                else if(mode==2) { o[0]=st[0]-sat(fade)*3; o[1]=st[1]-std::max(0.,std::min(3.,-o[0])); o[2]=st[2]-std::max(0.,std::min(3.,-o[1])); }
                else if(mode==3) { const double radd=sat(fade)*3, gadd=std::max(radd-1,0.), badd=std::max(gadd-1,0.); o[0]=st[0]+radd; o[1]=st[1]+gadd; o[2]=st[2]+badd; }
                else for(int c=0;c<3;c++) o[c]=st[c]+(en[c]-st[c])*fade;
                RGB result; for(int c=0;c<3;c++) result[c]=(int)std::lround(sat(o[c])*255.);
                return result;
            };
            // top-left 8, top-right 28, bottom-left 0, bottom-right 31 (lump rows from the bottom)
            auto setMask=[&]() { lumpBytes.assign(4000,0); for(int r=0;r<50;r++) for(int c=0;c<80;c++) lumpBytes[r*80+c]=(UINT8)(r>=25?(c<40?8:28):(c<40?0:31)); };
            const int maskTL=8, maskTR=28, maskBL=0, maskBR=31;
            auto runWipe=[&](UINT8 mode,bool reverse,int wiggle) {
                reset(S); startWipe(); reset(E); endWipe();
                g_wipemode=mode; g_wipetype=0; g_wipeframe=0; g_wipereverse=reverse; g_wipeencorewiggle=wiggle;
                doWipe(); fullWipes++;
            };
            setMask();
            const int shaderMode[4]={0,2,3,1}; // g_wipemode 0..3
            for(int wipemode=0;wipemode<4;wipemode++) for(int reverse=0;reverse<2;reverse++) {
                const char *scene=wipemode==0?"wipe-blend":"wipe-mode";
                runWipe((UINT8)wipemode,reverse!=0,0);
                auto p=pixels(); validate(validationQueue);
                const RGB start=reverse?E:S, end=reverse?S:E;
                check(scene,p,16,16,wiped(shaderMode[wipemode],start,end,maskTL,reverse!=0));
                check(scene,p,48,16,wiped(shaderMode[wipemode],start,end,maskTR,reverse!=0));
                check(scene,p,16,48,wiped(shaderMode[wipemode],start,end,maskBL,reverse!=0));
                check(scene,p,48,48,wiped(shaderMode[wipemode],start,end,maskBR,reverse!=0));
                D3D12_FinishUpdate(0);
            }
            // Encore's wiggle shifts each row sideways by a share of the screen; the start screen's edge shows where. Its mask
            // keeps all of the start screen (a byte of 31), in the to-black mode.
            {
                lumpBytes.assign(4000,31);
                const int wiggle=2;
                reset({0,0,0}); fillRect(-1,0,-1,1,tl); fillRect(0,1,-1,1,tr); startWipe(); reset(E); endWipe();
                g_wipemode=1; g_wipetype=0; g_wipeframe=0; g_wipereverse=0; g_wipeencorewiggle=wiggle;
                doWipe(); fullWipes++;
                auto p=pixels(); validate(validationQueue);
                int shifted=0;
                for(int y=0;y<H;y++) {
                    const double row=std::floor((1.0-(y+.5)/H)*200.0);
                    const double phase=wiggle*128.0+row*wiggle*8.0;
                    const double direction=(row-2.0*std::floor(row/2.0))*2.0-1.0;
                    const double shift=direction*std::sin(phase)*wiggle*8.0/320.0;
                    for(int x=0;x<W;x++) {
                        const double at=std::max(.5/W,std::min(1-.5/W,(x+.5)/W+shift))*W;
                        const double fraction=at-std::floor(at);
                        if(fraction<.03||fraction>.97) continue; // too near a texel's edge to tell float from double
                        const bool right=at>=W/2;
                        if(right!=(x>=W/2)) shifted++;
                        check("wipe-wiggle",p,x,y,right?tr:tl);
                    }
                }
                if(shifted<40) throw std::runtime_error("the wiggle shifted too few pixels to test");
                D3D12_FinishUpdate(0);
            }
            // The two-pass wipe, when the mask lump is missing: the end screen over the start screen, by a texture's alpha
            {
                auto mask=function<void (*)(GLMipmap_t *)>("SetTexture");
                auto deleteMask=function<void (*)(GLMipmap_t *)>("DeleteTexture");
                const UINT8 alpha[4]={64,192,0,255}; // top-left, top-right, bottom-left, bottom-right
                std::array<UINT8,4> alphaTexels={alpha[0],alpha[1],alpha[2],alpha[3]};
                GLMipmap_t fadeMask={}; fadeMask.format=GL_TEXFMT_ALPHA_8; fadeMask.width=fadeMask.height=2; fadeMask.data=alphaTexels.data();
                reset(S); startWipe(); reset(E); endWipe();
                mask(&fadeMask); // sets the fade mask the fallback reads
                g_wipemode=0; g_wipetype=0; g_wipeframe=2; g_wipereverse=0; g_wipeencorewiggle=0;
                doWipe(); twoPassWipes++;
                auto p=pixels(); validate(validationQueue);
                auto over=[&](int a) { RGB result; for(int c=0;c<3;c++) result[c]=(int)std::lround(E[c]*(a/255.)+S[c]*(1-a/255.)); return result; };
                check("wipe-fallback",p,16,16,over(alpha[0])); check("wipe-fallback",p,48,16,over(alpha[1]));
                check("wipe-fallback",p,16,48,over(alpha[2])); check("wipe-fallback",p,48,48,over(alpha[3]));
                deleteMask(&fadeMask);
                D3D12_FinishUpdate(0);
                if(logs.find("wipe: the full wipe isn't available")==std::string::npos) throw std::runtime_error("the two-pass wipe wasn't logged");
            }
            // A wipe's mask is remade at a new size, and the screen after a wipe still draws
            lumpBytes.assign(16000,0); g_wipeframe=1; // 160x100: every pixel of the end screen
            reset(S); startWipe(); reset(E); endWipe(); g_wipemode=0; g_wipereverse=0; g_wipeencorewiggle=0; doWipe(); fullWipes++;
            unbindTextures(); draw({20,200,40},PF_NoDepthTest);
            { auto p=pixels(); validate(validationQueue); check("wipe-after",p,32,32,{20,200,40}); check("wipe-after",p,2,2,E); }
            D3D12_FinishUpdate(0);
            g_wipeframe=0;
        }
        // Post image distortion samples the saved screen: not at all with none, a few pixels' sway with the water's
        {
            auto makeScreen=function<void (*)()>("MakeScreenTexture");
            auto postImage=function<void (*)(float [SCREENVERTS][SCREENVERTS][2])>("PostImgRedraw");
            auto clip=function<void (*)(INT32,INT32,INT32,INT32,float)>("GClipRect");
            static float grid[SCREENVERTS][SCREENVERTS][2]={};
            const RGB left={200,40,16}, right={16,64,224};
            clip(0,0,W,H,.1f);
            reset({0,0,0}); fillRect(-1,0,-1,1,left); fillRect(0,1,-1,1,right); makeScreen();
            reset({96,96,96}); postimgtype[0]=postimg_none; leveltime=0; postImage(grid);
            { auto p=pixels(); validate(validationQueue);
              for(int y=0;y<H;y+=7) { check("postimg-identity",p,31,y,left); check("postimg-identity",p,32,y,right); check("postimg-identity",p,5,y,left); check("postimg-identity",p,58,y,right); } }
            D3D12_FinishUpdate(0);
            // Water: each row's texture coordinate sways by a sine, up to 0.64 of a pixel here, which moves the edge at a pixel boundary
            const UINT32 ticks=40;
            reset({96,96,96}); postimgtype[0]=postimg_water; leveltime=ticks; postImage(grid);
            postimgtype[0]=postimg_none; leveltime=0;
            { auto p=pixels(); validate(validationQueue);
              int moved=0;
              for(int y=0;y<H;y++) {
                  const double shift=std::sin((ticks/35.0)*2.0+((y+.5)/H)*20.0)*0.01*W;
                  if(std::fabs(std::fabs(shift)-.5)<.03) continue; // too near a texel's edge to tell float from double
                  const bool leftMoves=shift>.5, rightMoves=shift<-.5;
                  moved+=leftMoves+rightMoves;
                  check("postimg-shift",p,31,y,leftMoves?right:left);
                  check("postimg-shift",p,32,y,rightMoves?left:right);
              }
              if(moved<6) throw std::runtime_error("the water's sway moved too few rows to test"); }
            D3D12_FinishUpdate(0);
        }
        // Fade maps: the screen's colors become palette indices, are remapped by a row of the table, and come back as the master palette's
        {
            auto setLookup=function<void (*)(UINT8 *)>("SetPaletteLookup");
            constexpr int lutSize=HWR_PALETTE_LUT_SIZE;
            std::vector<UINT8> lut(lutSize*lutSize*lutSize);
            for(int b=0;b<lutSize;b++) for(int g=0;g<lutSize;g++) for(int r=0;r<lutSize;r++) lut[(b*lutSize+g)*lutSize+r]=(r<lutSize/2 && g>=lutSize/2 && b>=lutSize/2)?7:19;
            auto rgba=[](RGB c) { RGBA_t v={}; v.s.red=(UINT8)c[0]; v.s.green=(UINT8)c[1]; v.s.blue=(UINT8)c[2]; v.s.alpha=255; return v; };
            std::array<RGBA_t,256> master; master.fill(rgba({8,8,8}));
            const RGB m40={200,100,50}, m41={30,220,90}, m43={90,20,200}, m7={8,8,8};
            master[40]=rgba(m40); master[41]=rgba(m41); master[43]=rgba(m43);
            pMasterPalette=master.data(); setLookup(lut.data());
            const RGB index7={24,180,220}, index19={200,40,16}; // lit colors: lookup index 7, then 19
            // Rows 0 to 2: the identity, 7 to 40 and 19 to 19, then 7 to 41 and 19 to 43
            std::vector<UINT8> table(3*256);
            for(int row=0;row<3;row++) for(int i=0;i<256;i++) table[row*256+i]=(UINT8)i;
            table[256+7]=40; table[512+7]=41; table[512+19]=43;
            // Palette rendering off leaves the screen alone, and tells the caller to fall back
            special(HWD_SET_PALETTE_RENDERING,0);
            reset(index7);
            if(D3D12_ApplyFadeMap(table.data(),table.size(),1234,1)) throw std::runtime_error("a fade map drew without palette rendering");
            expect("fade-map-off",index7);
            special(HWD_SET_PALETTE_RENDERING,1);
            reset(index7); if(!D3D12_ApplyFadeMap(table.data(),table.size(),1234,1)) throw std::runtime_error("the fade map didn't draw");
            expect("fade-map",m40);
            reset(index7); D3D12_ApplyFadeMap(table.data(),table.size(),1234,2); expect("fade-map",m41);
            reset(index7); D3D12_ApplyFadeMap(table.data(),table.size(),1234,0); expect("fade-map-row",m7); // the identity row
            reset(index7); D3D12_ApplyFadeMap(table.data(),table.size(),1234,9); expect("fade-map-clamp",m41); // the last row
            reset(index19); D3D12_ApplyFadeMap(table.data(),table.size(),1234,2); expect("fade-map",m43);
            // The same table id with four rows remakes its texture
            std::vector<UINT8> four(4*256); for(int row=0;row<4;row++) for(int i=0;i<256;i++) four[row*256+i]=(UINT8)i;
            four[3*256+7]=43;
            reset(index7); D3D12_ApplyFadeMap(four.data(),four.size(),1234,3); expect("fade-map-remake",m43);
            // Drawing after a fade map still works, and the screen keeps its fade-mapped color around the new quad
            unbindTextures();
            reset(index7); D3D12_ApplyFadeMap(table.data(),table.size(),1234,1); draw({20,200,40},PF_NoDepthTest);
            { auto p=pixels(); validate(validationQueue); check("fade-map-after",p,32,32,{20,200,40}); check("fade-map-after",p,2,2,m40); }
            D3D12_FinishUpdate(0);
            // A new table every frame retires the old one and its descriptor while recorded work reads them
            UINT64 placedBefore=0,blocksBefore=0,srvsBefore=0; D3D12_GetReuseCountsForTest(&placedBefore,&blocksBefore,&srvsBefore);
            for(int frame=0;frame<64;frame++) {
                std::vector<UINT8> remap((frame&1)?3*256:4*256); for(size_t i=0;i<remap.size();i++) remap[i]=(UINT8)(i&255);
                remap[(frame&1)?256+7:3*256+7]=(frame&1)?40:41;
                reset(index7); D3D12_ApplyFadeMap(remap.data(),remap.size(),5000+(UINT32)(frame>>1),(UINT8)((frame&1)?1:3));
                expect("fade-map-reuse",(frame&1)?m40:m41); D3D12_FinishUpdate(0);
            }
            UINT64 placedAfter=0,blocksAfter=0,srvsAfter=0; D3D12_GetReuseCountsForTest(&placedAfter,&blocksAfter,&srvsAfter);
            if(srvsAfter<=srvsBefore) throw std::runtime_error("fade map SRV reuse was not exercised");

            // A new screen size remakes the screen textures and the fade map's copy of the screen
            auto makeScreen=function<void (*)()>("MakeScreenTexture");
            auto drawIntermission=function<void (*)()>("DrawIntermissionBG");
            auto clip=function<void (*)(INT32,INT32,INT32,INT32,float)>("GClipRect");
            for(auto size:{std::array<int,2>{640,400},std::array<int,2>{W,H}}) {
                const int w=size[0], h=size[1];
                if(!D3D12_Surface(w,h)) throw std::runtime_error("resizing failed");
                clip(0,0,w,h,.1f);
                reset(index7); D3D12_ApplyFadeMap(table.data(),table.size(),1234,1);
                { RGB c=probe(w/2,h/2,h); if(negative=="resize-fade-map") c[0]=(c[0]+67)%256; if(abs(c[0]-m40[0])>1||abs(c[1]-m40[1])>1||abs(c[2]-m40[2])>1) { negativeRejected=negative=="resize-fade-map"; throw std::runtime_error("fade map after a resize"); } }
                quadrants(tl,tr,bl,br); makeScreen(); reset({96,96,96}); blend(PF_NoDepthTest); drawIntermission();
                const struct { int x, y; RGB want; } samples[]={{w/4,h/4,tl},{w*3/4,h/4,tr},{w/4,h*3/4,bl},{w*3/4,h*3/4,br}};
                for(auto &sample:samples) { RGB c=probe(sample.x,sample.y,h); RGB want=sample.want; if(negative=="resize-screen") want[0]=(want[0]+67)%256; if(abs(c[0]-want[0])>1||abs(c[1]-want[1])>1||abs(c[2]-want[2])>1) { negativeRejected=negative=="resize-screen"; throw std::runtime_error("screen texture after a resize"); } }
                validate(validationQueue); D3D12_FinishUpdate(0);
            }
            special(HWD_SET_PALETTE_RENDERING,0);
            pMasterPalette=nullptr; setLookup(lut.data());
            validate(validationQueue);
        }
        // Inverse of the sky's 270-degree Y rotation; height 200 keeps its Y scale at one.
        std::array<gl_skyvertex_t,4> skyVertices={{{1,-.8f,.8f,0,0,64,176,224,255},{1,-.8f,-.8f,1,0,64,176,224,255},{1,.8f,-.8f,1,1,64,176,224,255},{1,.8f,.8f,0,1,64,176,224,255}}};
        gl_skyloopdef_t skyLoop={HWD_SKYLOOP_FAN,4,0,false}; gl_sky_t sky={};
        sky.height=200; sky.loopcount=1; sky.vertex_count=4; sky.loops=&skyLoop; sky.data=skyVertices.data(); sky.rebuild=true;
        reset(); texture(nullptr); blend(PF_NoTexture|PF_NoDepthTest);
        function<void (*)(gl_sky_t *)>("RenderSkyDome")(&sky); expect("sky",{64,176,224});
        // A second rebuild exercises retirement of static sky storage.
        // SetTransform negates scalez; positive-Z fixtures need eye-space negative Z (clip w=-z).
        FTransform identity={}; identity.scalex=identity.scaley=1; identity.scalez=1; identity.angley=-270; identity.fovxangle=90;
        function<void (*)(FTransform *)>("SetTransform")(&identity);
        sky.rebuild=true; reset(); function<void (*)(gl_sky_t *)>("RenderSkyDome")(&sky); expect("sky",{64,176,224});
        function<void (*)(FTransform *)>("SetTransform")(&identity);
        std::swap(skyVertices[2],skyVertices[3]); skyLoop.mode=HWD_SKYLOOP_STRIP; sky.rebuild=true;
        reset(); function<void (*)(gl_sky_t *)>("RenderSkyDome")(&sky); expect("sky",{64,176,224});
        function<void (*)(FTransform *)>("SetTransform")(&identity);
        std::array<float,18> modelVertices={-.8f,-.8f,1,.8f,-.8f,1,.8f,.8f,1,-.8f,-.8f,1,.8f,.8f,1,-.8f,.8f,1};
        std::array<float,18> modelNext=modelVertices, normals={}; std::array<float,12> uvs={};
        for(int i=2;i<18;i+=3) normals[i]=1;
        for(int i=0;i<18;i+=3) modelNext[i]+=1.2f;
        mdlframe_t modelFrames[2]={}; modelFrames[0].vertices=modelVertices.data(); modelFrames[1].vertices=modelNext.data();
        modelFrames[0].normals=modelFrames[1].normals=normals.data();
        mesh_t mesh={}; mesh.numVertices=6; mesh.numTriangles=2; mesh.numFrames=2; mesh.frames=modelFrames; mesh.uvs=uvs.data();
        model_t model={}; model.numMeshes=1; model.meshes=&mesh; model.maxNumFrames=2; model.max_s=model.max_t=1;
        FTransform position={}; auto modelSurface=surface({192,96,48}); modelSurface.PolyFlags=PF_NoTexture|PF_NoDepthTest;
        special(HWD_SET_MODEL_LIGHTING,0);
        auto renderModel=function<void (*)(model_t *,INT32,float,float,INT32,FTransform *,float,float,UINT8,UINT8,FSurfaceInfo *)>("DrawModel");
        // The engine records the UV snapshot before creating model VBOs.
        model.vbo_max_s=model.max_s; model.vbo_max_t=model.max_t;
        function<void (*)(model_t *)>("CreateModelVBOs")(&model);
        if(!modelFrames[0].vboID || !modelFrames[1].vboID) throw std::runtime_error("model static buffers were not created");
        reset(); renderModel(&model,0,0,0,-1,&position,2,2,0,0,&modelSurface); expect("model-buffered",{192,96,48});
        reset(); renderModel(&model,0,2,1,1,&position,2,2,0,0,&modelSurface); expect("model-lerped",{192,96,48},50,32); expect("model-lerped",{48,80,112},14,32);
        model.max_s=.5f; // UV mismatch forces the dynamic-ring fallback.
        reset(); renderModel(&model,0,0,0,-1,&position,2,2,0,0,&modelSurface); expect("model-fallback",{192,96,48});
        for(int frame=0;frame<4;frame++) { reset(); for(int i=0;i<1000;i++) draw({(i&1)?80:120,40,160},PF_NoDepthTest); expect("rings",{80,40,160}); D3D12_FinishUpdate(0); }
        reset(); std::vector<FOutVector> huge(400,quad()[0]); huge[0]=quad()[0]; huge[1]=quad()[1]; huge[2]=quad()[2]; huge[3]=quad()[3]; auto hs=surface({32,220,96}); polygon(&hs,huge.data(),(FUINT)huge.size(),PF_Modulated|PF_NoTexture|PF_NoDepthTest); expect("growth",{32,220,96});
        D3D12_FinishUpdate(0); D3D12_LogPerf(1);
        if(logs.find("fallbacks 1, lerps 1")==std::string::npos) throw std::runtime_error("model buffered/lerped/fallback paths were not all exercised");
        { unsigned alive=9,uploads=0; int created=0,updated=0; double passes=0,ms=0; auto pp=logs.find("UWP perf:   D3D12: palette: rendering ");
          if(pp==std::string::npos) throw std::runtime_error("missing palette perf line");
          if(sscanf(logs.c_str()+pp,"UWP perf:   D3D12: palette: rendering %*s %u light tables alive, %d created, %d updated; %u LUT/palette uploads; %lf post passes per frame, %lf ms per frame",&alive,&created,&updated,&uploads,&passes,&ms)!=6
             || alive!=0 || created!=98 || updated!=3 || uploads<6 || passes<=0) throw std::runtime_error("palette perf counters are wrong"); }
        { int full=0,twoPass=0,images=0,fades=0,fallbacks=0; auto sp=logs.find("UWP perf:   D3D12: screens: ");
          if(sp==std::string::npos) throw std::runtime_error("missing screens perf line");
          if(sscanf(logs.c_str()+sp,"UWP perf:   D3D12: screens: %*f per frame (%*f copies, %*f barriers); wipes %d full, %d two-pass; post image %d; fade maps %d drawn, %d left to the overlay",&full,&twoPass,&images,&fades,&fallbacks)!=5
             || full!=fullWipes || twoPass!=twoPassWipes || images!=2 || fades!=73 || fallbacks!=1) throw std::runtime_error("screens perf counters are wrong");
          if(logs.find("not drawn yet: ")==std::string::npos || logs.find("postimage not drawn")!=std::string::npos) throw std::runtime_error("the not-drawn-yet line is wrong"); }
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
