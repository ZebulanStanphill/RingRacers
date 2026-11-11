// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
// Runs the production driver and GPU code; only engine services and presentation on the window are stubbed.
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
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "t_fsin.c"
#include "screen-effect-fixtures.h"
#undef min
#undef max
#undef clamp
using namespace rr_d3d12;
static_assert((PF_Invert & PF_Occlude) == 0, "Inversion must not alias depth writes");
// negative names the one negative control this process runs, if any: a regression is injected and the oracle that should
// catch it must then reject it. Every other oracle is checked by selfTest and the hit registry below.
static std::string logs, negative;
static bool negativeRejected=false;
static std::vector<UINT8> lumpBytes; // what the stubbed wipe mask lump holds
static std::map<std::string,std::string> shaderLumps; // what the stubbed shaders.pk3 holds: the screen effects' sources
extern "C" {
UINT32 hwr_roll_shader=0, hwr_roll_cpu=0, hwr_roll_geometry=0;
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
consvar_t cv_reducevfx{};
consvar_t cv_scr_effect{};
boolean W_ReadShader(const char *name, size_t *size, void *dest) {
    auto lump=shaderLumps.find(name);
    if(lump==shaderLumps.end()) return false;
    *size=lump->second.size();
    if(dest) memcpy(dest,lump->second.data(),lump->second.size());
    return true;
}
CV_PossibleValue_t glanisotropicmode_cons_t[] = {{0, "Off"}, {16, "16"}, {0, nullptr}};
void HWR_Startup() {}
UINT32 HWR_UWPTakeLazyModelLoads(void) { return 0; }
precise_t I_GetPreciseTime() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
UINT64 I_GetPrecisePrecision() { LARGE_INTEGER n; QueryPerformanceFrequency(&n); return n.QuadPart; }
void I_OutputMsg(const char *fmt, ...) { char b[8192]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap); logs += b; fputs(b,stdout); }
RGBA_t *pMasterPalette = nullptr;
void I_Error(const char *fmt, ...) { va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); exit(1); }
}
namespace rr_d3d12 {
bool Present_TakeWindow() { return true; }
static int presentedFrames=0;
void Present_Frame(ID3D12Resource *, D3D12_RESOURCE_STATES, UINT, UINT, bool, PlaceFrameFn) {
    presentedFrames++;
    // No transition is necessary: submission leaves the source in its supplied state.
    UINT64 fence = Core_Submit("present");
    if (!fence || !Core_Wait(fence, 30000, "headless present")) throw std::runtime_error("present failed");
}
void Present_ReleaseWindow() {}
void Present_Shutdown(bool) {}
void Present_GetTrace(INT32 *c,INT32 *s,UINT32 *cf,UINT32 *sf) { if(c)*c=-1; if(s)*s=-1; if(cf)*cf=0; if(sf)*sf=0; }
void Present_LogPerf(double) {}
bool Present_Tracing() { return false; }
void Present_FrameDone() {}
UINT64 Present_SwapChainBytes() { return 0; }
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
// Every oracle ends in one of the comparisons below. A rejection throws OracleRejected, which nothing else throws, so a
// negative control or the self-test can tell an oracle catching something from a crash or a validation error.
struct OracleRejected : std::runtime_error { using std::runtime_error::runtime_error; };
static std::map<std::string,unsigned> oracleHits; // how often each scene's oracle ran
static bool quietOracles=false; // the self-test rejects on purpose
[[noreturn]] static void reject(const char *scene,const char *why) { negativeRejected=negative==scene; throw OracleRejected(why); }
static bool within(RGB got,RGB want) { for(int c=0;c<3;c++) if(abs(got[c]-want[c])>1) return false; return true; }
static RGB at(const std::vector<UINT8> &p,int x,int y,int width=W) { const UINT8 *q=&p[(y*width+x)*3]; return {q[0],q[1],q[2]}; }
// The one pixel comparison: each channel may be one level off
static void same(const char *scene,const char *what,int x,int y,RGB got,RGB want) {
    oracleHits[scene]++;
    if(within(got,want)) return;
    if(!quietOracles) fprintf(stderr,"%s: %s %d,%d got %d,%d,%d expected %d,%d,%d\n",scene,what,x,y,got[0],got[1],got[2],want[0],want[1],want[2]);
    reject(scene,"pixel oracle failed");
}
// Every verdict that isn't a pixel
static void require(const char *scene,bool ok,const char *why) {
    oracleHits[scene]++;
    if(ok) return;
    if(!quietOracles) fprintf(stderr,"%s: %s\n",scene,why);
    reject(scene,why);
}
// A pixel of an image read earlier
static void check(const char *scene,const std::vector<UINT8> &p,int x,int y,RGB want,int width=W) { same(scene,"pixel",x,y,at(p,x,y,width),want); }
// A pixel of the screen
static void expect(const char *scene,RGB want,int x=32,int y=32) { auto p=pixels(); validate(validationQueue); check(scene,p,x,y,want); }
// A quad is two triangles split along the diagonal from its bottom left corner to its top right one. The screen's center, at NDC
// (0.0156,-0.0156), lies in the lower right triangle by half a pixel, so a regression that drops the upper left one passes
// every center-only check. (20,20) at NDC (-0.36,0.36) is in the upper left triangle and (44,44) at (0.39,-0.39) in the lower
// right one, a quarter of the screen from the diagonal; the quad spans -0.8 to 0.8, so all three are inside it.
static void expectQuad(const char *scene,RGB want) { auto p=pixels(); validate(validationQueue); check(scene,p,32,32,want); check(scene,p,20,20,want); check(scene,p,44,44,want); }
// Palette rendering's last step, read like expect reads the screen. It leaves the buffer as filled while Palette Rendering is off.
static std::vector<UINT8> palettePixels() { std::vector<UINT8> p(W*H*3,0xcd); D3D12_ReadPaletteRectForTest(0,0,W,H,W*3,reinterpret_cast<UINT16 *>(p.data())); return p; }
// What the driver hands to the present: the finished frame after palette rendering's last step and the screen effect
static std::vector<UINT8> presentedPixels() { std::vector<UINT8> p(W*H*3,0xcd); D3D12_ReadPresentedRectForTest(0,0,W,H,W*3,reinterpret_cast<UINT16 *>(p.data())); return p; }
// The frame as the driver places it on a screenWidth by screenHeight target for Screen Scaling mode 1 (Integer) or 2 (1:1), read
// whole. Inside the rectangle at (x0, y0) of size k times the frame's, every pixel is the frame's pixel at (x - x0) / k, (y - y0) / k;
// outside it, every pixel is black although the target was filled with magenta beforehand. The rectangle is worked out by hand in
// each scene, not by the production calculation.
static void expectPlaced(const char *scene,int mode,int screenWidth,int screenHeight,int x0,int y0,int k,const std::vector<UINT8> &frame) {
    std::vector<UINT8> p((size_t)screenWidth*screenHeight*3,0xcd);
    const bool placed=D3D12_ReadPlacedRectForTest(mode,screenWidth,screenHeight,0,0,screenWidth,screenHeight,screenWidth*3,reinterpret_cast<UINT16 *>(p.data()));
    validate(validationQueue);
    require(scene,placed,"the driver didn't place the frame");
    int wrong=0, black=0;
    for(int y=0;y<screenHeight;y++) for(int x=0;x<screenWidth;x++) {
        const bool inside=x>=x0 && x<x0+W*k && y>=y0 && y<y0+H*k;
        const RGB want=inside?at(frame,(x-x0)/k,(y-y0)/k):RGB{0,0,0};
        if(!within(at(p,x,y,screenWidth),want)) wrong++;
        else if(!inside) black++;
    }
    if(!quietOracles && wrong) fprintf(stderr,"%s: %d of %dx%d pixels aren't where the placement puts them\n",scene,wrong,screenWidth,screenHeight);
    require(scene,wrong==0,"a pixel of the placed frame or its border is wrong");
    require(scene,black==screenWidth*screenHeight-W*k*H*k,"the border wasn't checked whole");
}
// Modes that leave the frame to the system (Fit, or a frame larger than the screen) place nothing
static void expectNotPlaced(const char *scene,int mode,int screenWidth,int screenHeight) {
    std::vector<UINT8> p((size_t)screenWidth*screenHeight*3,0xcd);
    const bool placed=D3D12_ReadPlacedRectForTest(mode,screenWidth,screenHeight,0,0,screenWidth,screenHeight,screenWidth*3,reinterpret_cast<UINT16 *>(p.data()));
    validate(validationQueue);
    require(scene,!placed,"the driver placed a frame that is left to the system");
}
static unsigned occurrences(const std::string &text,const char *what) { unsigned n=0; for(size_t at=0;(at=text.find(what,at))!=std::string::npos;at+=strlen(what)) n++; return n; }
static void expectPost(const char *scene,RGB want,int x=32,int y=32) { auto p=palettePixels(); validate(validationQueue); same(scene,"palette pixel",x,y,at(p,x,y),want); }
// Some pixel of rows y0 to y1 in a column: a one-pixel line lies on a raster boundary, so which row holds it isn't pinned
static void checkAny(const char *scene,const std::vector<UINT8> &p,int x,int y0,int y1,RGB want,int width=W) {
    bool found=false; for(int y=y0;y<=y1;y++) found=found || within(at(p,x,y,width),want);
    require(scene,found,"no pixel of the rows matched");
}
static std::array<UINT64,3> captureCounts() { std::array<UINT64,3> result={}; D3D12_GetCaptureCountsForTest(&result[0],&result[1],&result[2]); return result; }
// Fog and water captures (fog, water copies, water round trips) since before
static void countDelta(const char *scene,std::array<UINT64,3> before,UINT64 fogCount,UINT64 waterCount) {
    const auto after=captureCounts();
    require(scene,after[0]-before[0]==fogCount && after[1]-before[1]==waterCount && after[2]-before[2]==waterCount,"capture copy/roundtrip delta failed");
}
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
// Every scene whose oracle the full run must reach. A scene never reached, or reached but missing here, fails the run, so a
// renamed, skipped or unlisted oracle can't go unnoticed.
static const char *const kOracles[]={
    "startup-present", "clear", "readrect", "surface-probe", "surface-probe-pixel", "surface-probe-quiet",
    "opaque", "translucent", "additive", "subtract", "reverse", "multiply", "environment", "masked", "invert", "fog",
    "depth", "nodepth", "noocclude", "portal-supported", "portal-aperture", "portal-nested", "portal-restore", "portal-depth", "portal-plane", "indexed", "line",
    "texture-guard", "texture-dedicated", "custom-shader", "custom-shader-clean", "custom-shader-invalid",
    "screen-effect-off", "screen-effect", "screen-effect-fallback", "screen-effect-palette",
    "scaling-integer", "scaling-1to1", "scaling-fallback", "scaling-effect",
    "texture", "texture-update", "texture-reuse", "mips", "shared-brightmap",
    "palette", "palette-post", "light-table-update", "light-table-update-post", "brightmap-palette", "fog-palette", "brightmap-smooth", "roll-sprite", "light-table-clear", "screen-palette", "palette-off",
    "fog-remap", "water-surface", "water-refraction", "water-group", "water-group-counts", "capture-mixed", "capture-recreate", "capture-skipped",
    "screen-copy", "final-screen", "screen-flush", "wipe-blend", "wipe-mode", "wipe-wiggle", "wipe-fallback", "wipe-after", "postimg-identity", "postimg-shift",
    "fade-map-off", "fade-map", "fade-map-row", "fade-map-clamp", "fade-map-remake", "fade-map-after", "fade-map-reuse", "resize-fade-map", "resize-screen",
    "sky", "model-buffered", "model-lerped", "model-fallback", "rings", "growth",
    "blend-invisible", "blend-invisible-depth", "blend-invisible-restore", "blend-remap-mask", "blend-remap-restore",
    "void-fill", "void-claim", "void-translucent", "void-depth", "void-viewport", "void-portal",
    "custom-none", "custom-layout-poly", "custom-layout-sky", "custom-layout-model", "custom-replace-invalid", "custom-replace-recover", "custom-sprite-clip", "custom-model-kept", "custom-model-lighting", "custom-water-refraction",
    "shutdown",
};
static std::string oracleProblems() {
    std::string problems;
    for(const char *name:kOracles) if(!oracleHits.count(name)) problems+=std::string(" unreached:")+name;
    for(const auto &entry:oracleHits)
        if(std::find_if(std::begin(kOracles),std::end(kOracles),[&](const char *name) { return entry.first==name; })==std::end(kOracles)) problems+=" unlisted:"+entry.first;
    return problems;
}
// Run once before the scenes. These oracles are all that stands between a regression and a green run, so each kind must accept
// what is right and reject, with its own message, what is wrong by more than its tolerance of one level.
template<class F> static void rejects(const char *what,const char *message,F run) {
    try { run(); }
    catch(const OracleRejected &e) {
        if(strcmp(e.what(),message)) throw std::runtime_error(std::string("oracle self-test: ")+what+" was rejected as \""+e.what()+"\", not \""+message+"\"");
        return;
    }
    throw std::runtime_error(std::string("oracle self-test: ")+what+" accepted a wrong expectation");
}
static void selfTest() {
    const auto savedHits=oracleHits;
    quietOracles=true;
    const char *const pixel="pixel oracle failed";
    const RGB c={40,100,200}, red={200,40,20};
    // expect and probe read the screen, which reset has just filled
    reset(c);
    expect("self-test",c); expect("self-test",{41,99,201});
    same("self-test","probe",32,32,probe(32,32,H),c);
    for(int channel=0;channel<3;channel++) {
        RGB wrong=c; wrong[channel]+=2;
        rejects("expect",pixel,[&] { expect("self-test",wrong); });
        rejects("probe",pixel,[&] { same("self-test","probe",32,32,probe(32,32,H),wrong); });
    }
    // The pixel asked for is the one compared: a quad leaves the corner as it was
    draw(red,PF_NoDepthTest);
    expect("self-test",red); expect("self-test",c,2,2);
    rejects("expect at a pixel the draw covers",pixel,[&] { expect("self-test",c); });
    rejects("expect at a pixel the draw missed",pixel,[&] { expect("self-test",red,2,2); });
    // The palette step's buffer is untouched while Palette Rendering is off (the palette-off scene pins that), so it reads 0xcd
    expectPost("self-test",{205,205,205}); expectPost("self-test",{204,206,205});
    rejects("expectPost",pixel,[&] { expectPost("self-test",{205,205,207}); });
    rejects("expectPost against the screen",pixel,[&] { expectPost("self-test",red); });
    // check, on a 2x2 image read earlier
    const std::vector<UINT8> image={10,20,30, 100,110,120, 50,60,70, 150,160,170};
    check("self-test",image,1,1,{150,160,170},2); check("self-test",image,0,1,{51,60,69},2);
    for(int channel=0;channel<3;channel++) {
        RGB wrong={150,160,170}; wrong[channel]-=2;
        rejects("check",pixel,[&] { check("self-test",image,1,1,wrong,2); });
    }
    rejects("check of another pixel",pixel,[&] { check("self-test",image,1,1,{50,60,70},2); });
    // checkAny, on a column of four pixels of which only the third matches
    const std::vector<UINT8> column={0,0,0, 0,0,0, 240,160,80, 0,0,0};
    checkAny("self-test",column,0,0,3,{240,160,80},1); checkAny("self-test",column,0,2,2,{241,160,80},1);
    rejects("checkAny outside the rows","no pixel of the rows matched",[&] { checkAny("self-test",column,0,0,1,{240,160,80},1); });
    rejects("checkAny of another color","no pixel of the rows matched",[&] { checkAny("self-test",column,0,0,3,{240,160,83},1); });
    // require, which carries every verdict that isn't a pixel
    require("self-test",true,"never thrown");
    rejects("require","a wrong verdict",[&] { require("self-test",false,"a wrong verdict"); });
    // countDelta: nothing is captured here, so no fog copy, water copy or round trip may be counted
    const auto before=captureCounts();
    countDelta("self-test",before,0,0);
    rejects("countDelta of a fog capture","capture copy/roundtrip delta failed",[&] { countDelta("self-test",before,1,0); });
    rejects("countDelta of a water capture","capture copy/roundtrip delta failed",[&] { countDelta("self-test",before,0,1); });
    // The hit registry lists every oracle that never ran, and one that isn't listed
    oracleHits.clear();
    if(oracleProblems().find(" unreached:startup-present")==std::string::npos) throw std::runtime_error("oracle self-test: the hit registry missed an unreached oracle");
    for(const char *name:kOracles) oracleHits[name]++;
    if(!oracleProblems().empty()) throw std::runtime_error("oracle self-test: the hit registry rejected a complete run");
    oracleHits["no-such-scene"]++;
    if(oracleProblems().find(" unlisted:no-such-scene")==std::string::npos) throw std::runtime_error("oracle self-test: the hit registry missed an unlisted oracle");
    oracleHits=savedHits; quietOracles=false;
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--negative-control")) negative=argv[2];
    else if(argc!=1) return 2;
    ComPtr<ID3D12Debug> debug; ComPtr<ID3D12Debug1> gbv;
    if(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || FAILED(debug.As(&gbv))) { fputs("D3D12 debug layer/GBV unavailable; install Windows Graphics Tools\n",stderr); return 77; }
    // A negative control injects a known regression (a shader mutant built by run-d3d12.ps1, or add-on shaders turned off) and
    // keeps the correct oracle, which must then reject it. The full run validates GPU state for all these scenes, so controls
    // skip GPU-based validation, which makes WARP several times slower.
    debug->EnableDebugLayer(); if(negative.empty()) gbv->SetEnableGPUBasedValidation(TRUE);
    puts(negative.empty() ? "D3D12 debug layer and GPU-based validation enabled" : "D3D12 debug layer enabled (negative control: GPU-based validation skipped)");
    try {
        Core_ForceWarp(true); D3D12_SetRingBytesForTest(1024,512,2048);
        if(!function<boolean (*)()>("Init")() || !D3D12_Surface(W,H)) throw std::runtime_error("Init/Surface failed");
        ComPtr<ID3D12InfoQueue> queue; if(FAILED(Core_Device()->QueryInterface(IID_PPV_ARGS(&queue)))) throw std::runtime_error("InfoQueue unavailable"); validationQueue=queue.Get();
        function<void (*)(INT32,INT32,INT32,INT32,float)>("GClipRect")(0,0,W,H,.1f);
        // Clear-only frames while the renderer starts aren't presented, so the loading screen stays up until the first draw.
        { reset(); D3D12_FinishUpdate(0); require("startup-present",presentedFrames==0,"clear-only startup frame was presented"); }
        selfTest();
        reset(); expect("clear",{48,80,112});
        reset({0,0,0});
        { auto upper=quad(); upper[0].y=upper[1].y=.1f; auto red=surface({248,32,16}); polygon(&red,upper.data(),4,PF_Modulated|PF_NoTexture|PF_NoDepthTest);
          auto lower=quad(); lower[2].y=lower[3].y=-.1f; auto blue=surface({16,64,248}); polygon(&blue,lower.data(),4,PF_Modulated|PF_NoTexture|PF_NoDepthTest);
          expect("readrect",{248,32,16},32,12); expect("readrect",{16,64,248},32,52);
          std::vector<UINT8> odd(7*5*3,0xcd); readrect(29,49,7,5,21,reinterpret_cast<UINT16 *>(odd.data()));
          bool oddRight=true; for(size_t i=0;i<odd.size();i++) oddRight=oddRight && abs(odd[i]-RGB{248,32,16}[i%3])<=1;
          require("readrect",oddRight,"odd-width/subrect RGB24 oracle failed");
          UINT16 packed[35]={}; readrect(29,49,7,5,14,packed); const UINT16 wanted565=(UINT16)((248>>3)<<11 | (32>>2)<<5 | (16>>3));
          bool packedRight=true; for(auto c:packed) packedRight=packedRight && c==wanted565;
          require("readrect",packedRight,"RGB565 oracle failed"); }
        // The surface probe copies the pixel under 50%,50% after each draw and logs the draws' colors at the end of the frame.
        { const RGB colors[3]={{200,40,20},{20,200,40},{40,20,200}};
          reset({0,0,0});
          if(D3D12_BeginSurfaceProbe()) throw std::runtime_error("a surface probe began without a request");
          if(!D3D12_RequestSurfaceProbe(50,50) || D3D12_RequestSurfaceProbe(50,50)) throw std::runtime_error("surface probe requests weren't accepted once");
          const size_t start=logs.size();
          if(!D3D12_BeginSurfaceProbe()) throw std::runtime_error("the surface probe didn't begin");
          for(int i=0;i<3;i++) { if(i==1) D3D12_SurfaceProbeSource("wall",7); draw(colors[i],PF_NoDepthTest); }
          D3D12_EndSurfaceProbe(); validate(validationQueue);
          const std::string probeLog=logs.substr(start);
          require("surface-probe",probeLog.find("surface probe: begin pixel=(32,32) screen=64x64")!=std::string::npos,"surface probe begin line");
          for(int i=0;i<3;i++)
          { char key[48]; snprintf(key,sizeof key,"surface probe: sample=%d raw=0x",i);
            const size_t entry=probeLog.find(key); unsigned raw=0; char kind[16]={}; int source=0;
            require("surface-probe",entry!=std::string::npos && sscanf(probeLog.c_str()+entry+strlen(key),"%x kind=%15s source=%d",&raw,kind,&source)==3,"surface probe sample line");
            // RGBA8 as a little-endian DWORD; like the other scenes, each color channel may be one off
            same("surface-probe","raw",32,32,{(int)(raw&255),(int)((raw>>8)&255),(int)((raw>>16)&255)},colors[i]);
            require("surface-probe",(raw>>24)==0xFF,"surface probe alpha");
            require("surface-probe",!strcmp(kind,i==1?"wall":"polygon") && source==(i==1?7:-1),"surface probe kind and source"); }
          require("surface-probe",probeLog.find("surface probe: end draws=3 color_changes=3 rows_omitted=0 overflow=0")!=std::string::npos,"surface probe end line");
          // The copies leave the screen as they found it
          expect("surface-probe-pixel",colors[2]);
          // Without a request, the next frame copies and logs nothing
          const size_t quiet=logs.size();
          reset(); if(D3D12_BeginSurfaceProbe()) throw std::runtime_error("a surface probe began twice for one request");
          draw(colors[0],PF_NoDepthTest); D3D12_EndSurfaceProbe(); expect("surface-probe-quiet",colors[0]);
          if(logs.find("surface probe:",quiet)!=std::string::npos) throw std::runtime_error("an inactive surface probe logged"); }
        RGB src={160,96,64},dst={48,80,112};
        struct Mode { const char *name; FBITFIELD flags; } modes[]={{"opaque",0},{"translucent",PF_Translucent},{"additive",PF_Additive},{"subtract",PF_Subtractive},{"reverse",PF_ReverseSubtract},{"multiply",PF_Multiplicative},{"environment",PF_Environment},{"masked",PF_Masked},{"invert",PF_Invert},{"fog",PF_Fog}};
        // The color of a source with this alpha drawn over a destination in one of these modes
        auto blended=[](FBITFIELD flags,RGB source,RGB target,int alpha) { const double a=alpha/255.; RGB result={}; for(int c=0;c<3;c++) { double s=source[c],d=target[c],v=s; switch(flags) { case PF_Translucent:v=s*a+d*(1-a);break; case PF_Additive:v=s*a+d;break; case PF_Subtractive:v=s*a-d;break; case PF_ReverseSubtract:v=d-s*a;break; case PF_Multiplicative:v=s*d/255;break; case PF_Environment:v=s+d*(1-a);break; case PF_Masked:v=s*a;break; case PF_Invert:v=s*(1-d/255);break; case PF_Fog:v=s*a+d*s/255;break; } result[c]=(int)std::lround(std::max(0.,std::min(255.,v))); } return result; };
        for(auto m:modes) { reset(dst); draw(src,m.flags|PF_NoDepthTest,1,128); expectQuad(m.name,blended(m.flags,src,dst,128)); D3D12_FinishUpdate(0); if(!presentedFrames) throw std::runtime_error("drawn frame was not presented"); }
        reset(); draw({200,40,20},PF_Occlude,.5f); draw({20,200,40},PF_Occlude,1.5f); expectQuad("depth",{200,40,20});
        draw({20,40,200},PF_NoDepthTest,1.5f); expectQuad("nodepth",{20,40,200});
        reset(); draw({200,40,20},0,.5f); draw({20,200,40},PF_Occlude,1.5f); expectQuad("noocclude",{20,200,40});
        // Portal recursion follows hw_main: increment the aperture, reset only its depth,
        // draw the child, then restore entrance depth while decrementing its stencil.
        {
            const RGB background={48,80,112}, child={24,180,220}, nested={224,64,32}, parent={32,220,96};
            auto aperture=[](float extent,float z) {
                return std::array<FOutVector,4>{{{-extent*z,-extent*z,z,0,0},{extent*z,-extent*z,z,0,0},
                    {extent*z,extent*z,z,0,0},{-extent*z,extent*z,z,0,0}}};
            };
            auto open=[&](const std::array<FOutVector,4> &v,int level,bool restore=false) {
                if(!D3D12_DrawPortalAperture(v.data(),level,restore)) throw std::runtime_error("portal aperture failed");
            };
            reset();
            require("portal-supported",D3D12_PortalsSupported()!=0,"portal PSO preflight failed");
            // Preflight and mask creation must preserve color; stencil == 1 confines the child.
            auto outer=aperture(.6f,.25f), inner=aperture(.25f,.2f);
            open(outer,0);
            { auto p=pixels(); validate(validationQueue); check("portal-aperture",p,32,32,background); }
            D3D12_SetPortalStencil(1); draw(child,PF_NoDepthTest);
            { auto p=pixels(); validate(validationQueue); check("portal-aperture",p,32,32,child); check("portal-aperture",p,8,32,background); }
            // Level 2 rejects the parent-only portion; restore returns it to level 1.
            open(inner,1); D3D12_SetPortalStencil(2); draw(nested,PF_NoDepthTest);
            { auto p=pixels(); validate(validationQueue); check("portal-nested",p,32,32,nested); check("portal-nested",p,46,32,child); check("portal-nested",p,8,32,background); }
            D3D12_SetPortalStencil(1); open(inner,1,true); draw(parent,PF_NoDepthTest);
            { auto p=pixels(); validate(validationQueue); check("portal-nested",p,32,32,parent); check("portal-nested",p,46,32,parent); check("portal-nested",p,8,32,background); }
            // The entrance depth restored on return blocks farther parent geometry.
            D3D12_SetPortalStencil(0); open(outer,0,true); draw(nested,PF_Occlude,1.5f);
            { auto p=pixels(); validate(validationQueue); check("portal-restore",p,32,32,parent); check("portal-restore",p,8,32,nested); }
            // A global depth clear would wrongly reveal the far draw outside the aperture.
            reset(); draw(parent,PF_Occlude,.5f); open(outer,0);
            D3D12_ResetPortalDepth(1); D3D12_SetPortalStencil(0); draw(child,PF_Occlude,1.5f);
            { auto p=pixels(); validate(validationQueue); check("portal-depth",p,32,32,child); check("portal-depth",p,8,32,parent); }
            // The world exit plane accepts x >= 0; disabling it must restore both sides.
            reset(); const float plane[4]={1,0,0,0}; D3D12_SetPortalPlane(plane);
            draw(child,PF_NoDepthTest);
            { auto p=pixels(); validate(validationQueue); check("portal-plane",p,16,32,background); check("portal-plane",p,48,32,child); }
            D3D12_SetPortalPlane(nullptr); draw(parent,PF_NoDepthTest);
            { auto p=pixels(); validate(validationQueue); check("portal-plane",p,16,32,parent); check("portal-plane",p,48,32,parent); }
            D3D12_SetPortalStencil(0); D3D12_FinishUpdate(0);
        }
        reset(); auto v=quad(); auto s=surface({24,180,220}); UINT32 ix[]={0,1,2,0,2,3}; indexed(&s,v.data(),6,PF_Modulated|PF_NoTexture|PF_NoDepthTest,ix); expectQuad("indexed",{24,180,220});
        reset(); blend(PF_NoDepthTest|PF_NoTexture); F2DCoord p1={-.75f,0},p2={.75f,0}; auto ls=surface({240,160,80}); line(&p1,&p2,ls.PolyColor);
        // A one-pixel line lies on a raster boundary; inspect its known interior coverage, excluding endpoints.
        { auto p=pixels(); validate(queue.Get()); checkAny("line",p,32,30,33,{240,160,80}); }
        auto texture = function<void (*)(GLMipmap_t *)>("SetTexture");
        auto updateTexture = function<void (*)(GLMipmap_t *)>("UpdateTexture");
        auto deleteTexture = function<void (*)(GLMipmap_t *)>("DeleteTexture");
        auto special = function<void (*)(hwdspecialstate_t,INT32)>("SetSpecialState");
        special(HWD_SET_TEXTUREFILTERMODE,HWD_SET_TEXTUREFILTER_POINTSAMPLED);
        // Screen copies are bound to unit 0 without telling SetTexture's bookkeeping, so unbind it
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
        // M is both A's brightmap and the next surface's own texture. No reset,
        // readback or unrelated bind may repair the unit-0 cache between the draws.
        {
            std::array<UINT8,4> basePixel={216,48,24,255}, mapPixel={32,200,80,255};
            GLMipmap_t a={}, m={};
            for(auto *t:{&a,&m}) { t->format=GL_TEXFMT_RGBA; t->width=t->height=1; }
            a.data=basePixel.data(); m.data=mapPixel.data();
            texture(&a); texture(&m); // Make both resident before the cache transition.
            auto shader=function<void (*)(int)>("SetShader");
            special(HWD_SET_SHADERS,HWD_SHADEROPTION_ON); shader(SHADER_SPRITE);
            auto sharedDraw=[&](float left,float right) {
                constexpr float depth=128.125f;
                auto q=quad(depth); q[0].x=q[3].x=left*depth; q[1].x=q[2].x=right*depth;
                auto white=surface({255,255,255}); white.LightInfo.light_level=255; white.LightInfo.fade_end=31;
                polygon(&white,q.data(),4,PF_Modulated|PF_ColorMapped|PF_NoDepthTest);
            };
            for(bool upload:{false,true}) {
                reset();
                texture(&a);
                m.flags|=TF_BRIGHTMAP;
                if(upload) updateTexture(&m); else texture(&m);
                sharedDraw(-.8f,-.1f);
                m.flags&=~TF_BRIGHTMAP;
                texture(&m); sharedDraw(.1f,.8f);
                expect("shared-brightmap",{216,48,24},18,32);
                expect("shared-brightmap",{32,200,80},46,32);
                D3D12_FinishUpdate(0);
            }
            deleteTexture(&a); deleteTexture(&m);
            function<void (*)()>("UnSetShader")(); special(HWD_SET_SHADERS,HWD_SHADEROPTION_OFF);
        }
        // Query production placements only after ordinary SetTexture creation/upload.
        // Each block must contain its own complete blind guard, including mixed alignments.
        struct Placement { UINT32 heap; UINT64 offset, block, reported, alignment; };
        {
            std::array<GLMipmap_t,4> textures={};
            std::array<std::vector<UINT8>,4> data;
            std::array<Placement,4> placements={};
            for(size_t i=0;i<textures.size();i++) {
                const int side=i<2?256:64;
                const RGB color=i%2?RGB{16,200,80}:RGB{200,40,16};
                data[i].resize(side*side*4);
                for(size_t pixel=0;pixel<data[i].size()/4;pixel++) {
                    for(size_t c=0;c<3;c++) data[i][pixel*4+c]=(UINT8)color[c];
                    data[i][pixel*4+3]=255;
                }
                auto &t=textures[i]; t.format=GL_TEXFMT_RGBA; t.width=t.height=side; t.data=data[i].data();
                texture(&t);
                auto &p=placements[i];
                if(!D3D12_GetTexturePlacementForTest(t.downloaded,&p.heap,&p.offset,&p.block,&p.reported,&p.alignment))
                    throw std::runtime_error("texture-guard did not use placed storage");
                const UINT64 alignment=i<2?65536:4096;
                const UINT64 expectedBlock=i<2?320*1024:20*1024;
                if(p.alignment!=alignment || p.offset%alignment || p.block%alignment
                    || p.reported>p.block || p.block-p.reported<4096 || p.block!=expectedBlock)
                    throw std::runtime_error("texture-guard allocation bounds/class oracle failed");
            }
            // Fresh same-class pairs are adjacent; the next resource begins after the entire guard.
            for(size_t first : {size_t(0),size_t(2)}) {
                const auto &a=placements[first], &b=placements[first+1];
                if(a.heap!=b.heap || b.offset!=a.offset+a.block || b.offset<a.offset+a.reported+4096)
                    throw std::runtime_error("texture-guard neighboring allocations share a guard");
            }
            if(placements[0].heap==placements[2].heap)
                throw std::runtime_error("texture-guard mixed alignments shared a chunk");
            for(size_t i=0;i<textures.size();i++) {
                reset(); texture(&textures[i]); textured();
                expect("texture-guard",i%2?RGB{16,200,80}:RGB{200,40,16});
                deleteTexture(&textures[i]);
            }
            D3D12_FinishUpdate(0);
        }
        // A 1024x1024 texture exceeds the shared heaps' largest class once its guard is counted. It must get a heap
        // of its own rather than committed storage, whose last texel the Xbox One driver's state word can zero.
        {
            UINT64 placedBefore=0,dedicatedBefore=0,committedBefore=0;
            D3D12_GetTextureStorageCountsForTest(&placedBefore,&dedicatedBefore,&committedBefore);
            std::vector<UINT8> big(1024*1024*4);
            for(size_t pixel=0;pixel<big.size()/4;pixel++) { big[pixel*4]=40; big[pixel*4+1]=96; big[pixel*4+2]=216; big[pixel*4+3]=255; }
            // The bottom-right texel differs so that a zeroed or clamped last texel would show.
            big[big.size()-4]=232; big[big.size()-3]=200; big[big.size()-2]=24;
            GLMipmap_t t={}; t.format=GL_TEXFMT_RGBA; t.width=t.height=1024; t.data=big.data();
            reset(); texture(&t);
            Placement p={};
            if(D3D12_GetTexturePlacementForTest(t.downloaded,&p.heap,&p.offset,&p.block,&p.reported,&p.alignment))
                throw std::runtime_error("texture-dedicated used a shared heap block");
            UINT64 placed=0,dedicated=0,committed=0;
            D3D12_GetTextureStorageCountsForTest(&placed,&dedicated,&committed);
            if(placed!=placedBefore || dedicated!=dedicatedBefore+1 || committed!=committedBefore)
                throw std::runtime_error("texture-dedicated storage counts oracle failed");
            // Draw with texel (1023,1023) alone on the top-right pixel: s and t of 1 sample the last texel with point filtering.
            auto q=quad(); q[0].x=q[3].x=-1; q[1].x=q[2].x=1; q[0].y=q[1].y=-1; q[2].y=q[3].y=1;
            q[0].s=q[3].s=0; q[1].s=q[2].s=1.f+1.f/W; q[0].t=q[1].t=0; q[2].t=q[3].t=1.f+1.f/H;
            auto white=surface({255,255,255}); const FBITFIELD flags=PF_Modulated|PF_NoDepthTest;
            blend(flags); polygon(&white,q.data(),4,flags);
            expect("texture-dedicated",{40,96,216},8,32); expect("texture-dedicated",{232,200,24},W-1,0);
            deleteTexture(&t); D3D12_FinishUpdate(0);
        }
        // Add-on shaders use the Legacy GL loader and replace a valid base slot.
        // NOCUSTOM must make the same custom-color oracle fail with built-in pixels.
        {
            auto load=function<void (*)(int,char *,size_t,boolean)>("LoadCustomShader");
            auto compile=function<boolean (*)()>("CompileShaders");
            auto clean=function<void (*)()>("CleanShaders");
            auto shader=function<void (*)(int)>("SetShader");
            char vertex[]="void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }";
            char fragment[]="void main() { gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0); }";
            std::array<UINT8,4> color={200,40,16,255};
            GLMipmap_t customTexture={}; customTexture.format=GL_TEXFMT_RGBA;
            customTexture.width=customTexture.height=1; customTexture.data=color.data();
            auto customDraw=[&]() {
                reset(); texture(&customTexture); auto q=quad();
                auto white=surface({255,255,255}); white.LightInfo.light_level=255; white.LightInfo.fade_end=31;
                const FBITFIELD flags=PF_Modulated|PF_NoDepthTest;
                blend(flags); polygon(&white,q.data(),4,flags);
            };
            special(HWD_SET_SHADERS,HWD_SHADEROPTION_ON);
            load(SHADER_FLOOR,vertex,sizeof(vertex)-1,false);
            load(SHADER_FLOOR,fragment,sizeof(fragment)-1,true);
            if(!compile()) throw std::runtime_error("custom shader compilation failed");
            shader(SHADER_FLOOR);
            if(negative=="custom-shader-nocustom") special(HWD_SET_SHADERS,HWD_SHADEROPTION_NOCUSTOM);
            customDraw();
            try { expect("custom-shader",{64,128,191}); }
            catch(const OracleRejected &) { if(negative=="custom-shader-nocustom") negativeRejected=true; throw; }
            clean(); shader(SHADER_FLOOR); customDraw(); expect("custom-shader-clean",{200,40,16});
            const size_t logStart=logs.size();
            char invalid[]="void main() { gl_FragColor = vec4(; }";
            load(SHADER_FLOOR,vertex,sizeof(vertex)-1,false);
            load(SHADER_FLOOR,invalid,sizeof(invalid)-1,true);
            if(!compile()) throw std::runtime_error("invalid add-on disabled built-in shaders");
            const std::string diagnostic=logs.substr(logStart);
            if(diagnostic.find("custom shader")==std::string::npos || diagnostic.find("built-in")==std::string::npos)
                throw std::runtime_error("invalid custom shader did not log built-in fallback");
            shader(SHADER_FLOOR); customDraw(); expect("custom-shader-invalid",{200,40,16});
            clean(); shader(SHADER_NONE); special(HWD_SET_SHADERS,HWD_SHADEROPTION_OFF);
            deleteTexture(&customTexture);
        }
        // The SalCRT screen effects. The game's shaders are game data, so the lumps are stand-ins that use the same RHI macros;
        // the stub W_ReadShader serves them. SalCRT's work: through the glsllist (with a comment, CRLF and a blank line, and an
        // entry that already ends in .glsl), its translation and compilation, and the pass, whose output is the frame's red,
        // half its green (the frame's width over 128) and 128 (the dot pattern's red, read through a repeating sampler, times
        // the frame's height over 64). SalCRT Sharp's fragment doesn't translate: its frame is shown as it is, logged once.
        {
            shaderLumps["rhi_glsllist_crt_fragment.txt"]="# stand-in sources\r\nrhi_glsl_lib_versioncompat\r\n\r\nrhi_glsl_fragment_crt.glsl\n";
            shaderLumps["rhi_glsl_lib_versioncompat.glsl"]=kStandInCompat;
            shaderLumps["rhi_glsl_fragment_crt.glsl"]=kStandInEffect;
            shaderLumps["rhi_glsllist_crtsharp_fragment.txt"]="rhi_glsl_lib_versioncompat\nrhi_glsl_fragment_crtsharp\n";
            shaderLumps["rhi_glsl_fragment_crtsharp.glsl"]="void main() { OUTPUT_COLOR = vec4(; }";
            const RGB tl={200,80,16}, tr={40,160,16}, bl={120,240,16}, br={240,20,16};
            const RGB shadedTl={200,40,128}, shadedTr={40,80,128}, shadedBl={120,120,128}, shadedBr={240,10,128};
            // The pixels at the middle of the screen's four quarters (images read back have their top row first)
            auto corners=[&](const char *scene,const std::vector<UINT8> &p,RGB a,RGB b,RGB c,RGB d) {
                check(scene,p,16,16,a); check(scene,p,48,16,b); check(scene,p,16,48,c); check(scene,p,48,48,d);
            };
            cv_scr_effect.value=0; quadrants(tl,tr,bl,br);
            { auto p=presentedPixels(); validate(validationQueue); corners("screen-effect-off",p,tl,tr,bl,br); }
            const size_t effectLog=logs.size();
            cv_scr_effect.value=2; quadrants(tl,tr,bl,br);
            { auto p=presentedPixels(); validate(validationQueue); corners("screen-effect",p,shadedTl,shadedTr,shadedBl,shadedBr); }
            // A presented frame, and a second pass: nothing is built again
            D3D12_FinishUpdate(0); quadrants(tl,tr,bl,br);
            { auto p=presentedPixels(); validate(validationQueue); corners("screen-effect",p,shadedTl,shadedTr,shadedBl,shadedBr); }
            const std::string effectText=logs.substr(effectLog);
            require("screen-effect",occurrences(effectText,"screen effect SalCRT is ready")==1 && effectText.find("SalCRT failed")==std::string::npos,"SalCRT was not built exactly once");
            // Not the shaders of the other mode, either
            cv_scr_effect.value=3; quadrants(tl,tr,bl,br);
            for(int frame=0;frame<2;frame++) {
                auto p=presentedPixels(); validate(validationQueue); corners("screen-effect-fallback",p,tl,tr,bl,br); D3D12_FinishUpdate(0); quadrants(tl,tr,bl,br);
            }
            const std::string fallbackText=logs.substr(effectLog);
            require("screen-effect-fallback",occurrences(fallbackText,"screen effect SalCRT Sharp failed: translation failed")==1 && fallbackText.find("SalCRT Sharp is ready")==std::string::npos,"SalCRT Sharp's failure was not logged once");
            // Back to the first mode, which stays built; with the effect off the frame is as drawn
            cv_scr_effect.value=2;
            { auto p=presentedPixels(); validate(validationQueue); corners("screen-effect",p,shadedTl,shadedTr,shadedBl,shadedBr); }
            cv_scr_effect.value=1;
            { auto p=presentedPixels(); validate(validationQueue); corners("screen-effect-off",p,tl,tr,bl,br); }
            cv_scr_effect.value=0;
        }
        // Screen Scaling's Integer and 1:1: the frame of four quarters is drawn point-sampled at k times its size in the middle of a
        // screen-sized target, which is black around it, or left to the system when it doesn't fit. The target is filled with magenta
        // first, so a missing clear shows. (x, y) is the rectangle's top left, floor((screen - size) / 2); an odd remainder leaves the
        // larger margin on the right and bottom.
        {
            const RGB tl={200,80,16}, tr={40,160,16}, bl={120,240,16}, br={240,20,16};
            cv_scr_effect.value=0; quadrants(tl,tr,bl,br);
            const std::vector<UINT8> frame=presentedPixels(); validate(validationQueue);
            check("scaling-integer",frame,16,16,tl); check("scaling-integer",frame,48,16,tr); check("scaling-integer",frame,16,48,bl); check("scaling-integer",frame,48,48,br);
            expectPlaced("scaling-integer",1,128,128,0,0,2,frame);   // exactly twice the frame
            expectPlaced("scaling-integer",1,201,151,36,11,2,frame); // 151 / 64 is 2; 201 - 128 and 151 - 128 are odd
            expectPlaced("scaling-integer",1,200,200,4,4,3,frame);   // 192 of 200
            expectPlaced("scaling-integer",1,400,130,136,1,2,frame); // the height decides: 130 / 64 is 2
            expectPlaced("scaling-integer",1,64,64,0,0,1,frame);     // k is 1: the frame as it is
            expectPlaced("scaling-integer",1,70,64,3,0,1,frame);     // a wider screen
            expectPlaced("scaling-integer",1,64,130,0,33,1,frame);   // a taller screen: the width decides, 64 / 64 is 1
            expectPlaced("scaling-1to1",2,101,90,18,13,1,frame);     // 37 and 26 left over
            expectPlaced("scaling-1to1",2,400,300,168,118,1,frame);  // however large the screen, not scaled
            expectPlaced("scaling-1to1",2,64,64,0,0,1,frame);
            // The frame doesn't fit the screen in one dimension or both: left to the system's fit, as is Fit itself
            expectNotPlaced("scaling-fallback",1,63,100); expectNotPlaced("scaling-fallback",1,100,63); expectNotPlaced("scaling-fallback",1,40,40);
            expectNotPlaced("scaling-fallback",2,63,100); expectNotPlaced("scaling-fallback",2,100,63); expectNotPlaced("scaling-fallback",2,40,40);
            expectNotPlaced("scaling-fallback",0,200,200); expectNotPlaced("scaling-fallback",0,63,100);
            // The screen effect runs at the frame's size, and its result is what is placed
            const RGB shadedTl={200,40,128}, shadedTr={40,80,128}, shadedBl={120,120,128}, shadedBr={240,10,128};
            cv_scr_effect.value=2; quadrants(tl,tr,bl,br);
            const std::vector<UINT8> shaded=presentedPixels(); validate(validationQueue);
            check("scaling-effect",shaded,16,16,shadedTl); check("scaling-effect",shaded,48,16,shadedTr); check("scaling-effect",shaded,16,48,shadedBl); check("scaling-effect",shaded,48,48,shadedBr);
            expectPlaced("scaling-effect",1,201,151,36,11,2,shaded);
            cv_scr_effect.value=0;
        }
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
        // Compare the integer inverse-map shader against a CPU-rasterized patch,
        // drawn by the ordinary masked sprite path. Native tests independently
        // compare this mapping and geometry against production DoRotation.
        {
            auto shader=function<void (*)(int)>("SetShader");
            special(HWD_SET_SHADERS,HWD_SHADEROPTION_ON); shader(SHADER_SPRITE);
            require("roll-sprite",D3D_CanRollSprite()!=0,"nearest built-in sprite shader rejected");
            special(HWD_SET_TEXTUREFILTERMODE,HWD_SET_TEXTUREFILTER_BILINEAR);
            require("roll-sprite",!D3D_CanRollSprite(),"linear filtering must use CPU rotation");
            special(HWD_SET_TEXTUREFILTERMODE,HWD_SET_TEXTUREFILTER_POINTSAMPLED);
            constexpr int sw=9,sh=11,cw=32,ch=32;
            std::vector<UINT8> base(sw*sh*4),bright(sw*sh*4),rot(cw*ch*4),rotbright(cw*ch*4);
            for(int y=0;y<sh;y++) for(int x=0;x<sw;x++) {
                int o=(y*sw+x)*4;
                base[o]=(UINT8)(20+x*22); base[o+1]=(UINT8)(16+y*19); base[o+2]=160;
                base[o+3]=(x+y)%4?255:0;
                bright[o]=bright[o+1]=bright[o+2]=x%3?0:255; bright[o+3]=255;
            }
            auto mip=[](int w,int h,std::vector<UINT8> &bytes,bool bm) {
                GLMipmap_t m={}; m.format=GL_TEXFMT_RGBA; m.width=(UINT16)w; m.height=(UINT16)h;
                m.data=bytes.data(); m.flags=bm?TF_BRIGHTMAP:0; return m;
            };
            auto bm=mip(sw,sh,base,false),br=mip(sw,sh,bright,true);
            auto rm=mip(cw,ch,rot,false),rr=mip(cw,ch,rotbright,true);
            for(bool flip:{false,true}) for(int angle:{1,9,18,27,53,71}) {
                double radians=-angle*5*3.14159265358979323846/180;
                int ca=(int)(cos(radians)*65536),sa=(int)(sin(radians)*65536);
                int xc=(flip?sw-2:2)*65536,yc=3*65536; // explicit off-centre pivot
                std::fill(rot.begin(),rot.end(),0); std::fill(rotbright.begin(),rotbright.end(),0);
                for(int y=0;y<ch;y++) for(int x=0;x<cw;x++) {
                    // Software FixedMul's 64-bit intermediate, before the final shift.
                    auto mul=[](int a,int b) { return (int)(((INT64)a*b)>>16); };
                    int dx=(x-cw/2)*65536,dy=(y-ch/2)*65536;
                    int sx=(mul(dx,ca)+mul(dy,sa)+xc)>>16;
                    int sy=(-mul(dx,sa)+mul(dy,ca)+yc)>>16;
                    if(sx<0 || sy<0 || sx>=sw || sy>=sh) continue;
                    if(flip) sx=sw-1-sx;
                    memcpy(&rot[(y*cw+x)*4],&base[(sy*sw+sx)*4],4);
                    memcpy(&rotbright[(y*cw+x)*4],&bright[(sy*sw+sx)*4],4);
                }
                updateTexture(&rm); updateTexture(&rr);
                auto surfaceInfo=surface({255,255,255}); surfaceInfo.LightInfo.light_level=96;
                surfaceInfo.LightInfo.fade_end=31;
                auto q=quad(128.125f);
                q[1].s=q[2].s=1; q[2].t=q[3].t=1;
                const FBITFIELD f=PF_Modulated|PF_ColorMapped|PF_NoDepthTest;
                reset(); texture(&rm); texture(&rr); polygon(&surfaceInfo,q.data(),4,f);
                auto expected=pixels();
                INT32 parameters[16]={ca,sa,xc,yc,cw,ch,0,0,sw,sh,flip?1:0,1,0,0,0,0};
                memcpy(surfaceInfo.roll,parameters,sizeof(parameters));
                float scale[2]={1.0f/sw,1.0f/sh}; memcpy(surfaceInfo.roll+12,scale,sizeof(scale));
                q[1].s=q[2].s=(float)cw; q[2].t=q[3].t=(float)ch;
                reset(); texture(&bm); texture(&br); polygon(&surfaceInfo,q.data(),4,f|PF_RollSprite);
                auto actual=pixels(); validate(validationQueue);
                for(int y=0;y<H;y++) for(int x=0;x<W;x++)
                    same("roll-sprite","pixel",x,y,at(actual,x,y),at(expected,x,y));
                D3D12_FinishUpdate(0);
            }
            deleteTexture(&bm); deleteTexture(&br); deleteTexture(&rm); deleteTexture(&rr);
            shader(SHADER_NONE); special(HWD_SET_SHADERS,HWD_SHADEROPTION_OFF);
        }
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
            // The screen effect reads the palette step's texture, not the screen
            cv_scr_effect.value=2;
            { auto p=presentedPixels(); validate(validationQueue); same("screen-effect-palette","pixel",32,32,at(p,32,32),{index19[0],index19[1]/2,128}); }
            cv_scr_effect.value=0;
            // Presenting again repeats the pass on the same screen
            D3D12_FinishUpdate(0);
            // b) One frame, no readback between the draws: the first keeps the old table, the second gets the new one.
            reset(); paletteDraw(lightId,-.8f,-.1f);
            lightTable[7]=rgba(newColor); updateLightTable(lightId,lightTable.data());
            paletteDraw(lightId,.1f,.8f);
            expect("light-table-update",oldColor,18,32); expect("light-table-update",newColor,46,32);
            expectPost("light-table-update-post",index19,18,32); expectPost("light-table-update-post",index7,46,32);
            D3D12_FinishUpdate(0);
            // Bright texels use the base full-bright row (32), never the surface row. The four texel colors stand for palette
            // indices as the cache hands them over, already translated for Encore (the native encore_brightmap test checks that
            // translation), so what is drawn here is only the shader's choice of row.
            {
                const RGB brightColors[]={{24,220,76},{220,64,144},{64,112,232},{232,192,32}};
                const RGB darkColors[]={{72,96,120},{128,80,48},{48,112,80},{112,48,96}};
                const RGB texelColors[]={{24,180,220},{180,60,80},{24,60,80},{180,180,220}};
                const int indices[]={7,19,31,43};
                auto brightLut=lut;
                for(int b=0;b<lutSize;b++) for(int g=0;g<lutSize;g++) for(int r=0;r<lutSize;r++)
                    brightLut[(b*lutSize+g)*lutSize+r]=(UINT8)(r<lutSize/2?(g>=lutSize/2?7:31):(g<lutSize/2?19:43));
                setLookup(brightLut.data());
                for(int row=0;row<33;row++) for(int k=0;k<4;k++)
                    lightTable[row*256+indices[k]]=rgba(row==32?brightColors[k]:darkColors[k]);
                updateLightTable(lightId,lightTable.data());
                std::array<UINT8,4> brightTexel={255,255,255,255};
                GLMipmap_t brightTexture={}; brightTexture.format=GL_TEXFMT_RGBA;
                brightTexture.width=brightTexture.height=1; brightTexture.flags=TF_BRIGHTMAP;
                brightTexture.data=brightTexel.data();
                setShader(SHADER_SPRITE);
                for(int index=0;index<4;index++) for(bool bright:{false,true})
                for(int lighting:{64,160}) for(float distance:{32.125f,128.125f,256.125f}) {
                    for(int c=0;c<3;c++) paletteTexel[c]=(UINT8)texelColors[index][c];
                    brightTexel[0]=brightTexel[1]=brightTexel[2]=bright?255:0;
                    updateTexture(&paletteTexture); updateTexture(&brightTexture);
                    reset(); texture(&paletteTexture); texture(&brightTexture);
                    auto bs=surface({255,255,255}); bs.LightTableId=lightId;
                    bs.LightInfo.light_level=lighting; bs.LightInfo.fade_end=31;
                    auto q=quad(distance);
                    polygon(&bs,q.data(),4,PF_Modulated|PF_ColorMapped|PF_NoDepthTest);
                    expect("brightmap-palette",bright?brightColors[index]:darkColors[index]);
                    D3D12_FinishUpdate(0);
                }
                // The smooth path must retain the same pretranslated colors at full light.
                special(HWD_SET_PALETTE_RENDERING,0);
                for(int index=0;index<4;index++) for(bool bright:{false,true}) {
                    for(int c=0;c<3;c++) paletteTexel[c]=(UINT8)texelColors[index][c];
                    brightTexel[0]=brightTexel[1]=brightTexel[2]=bright?255:0;
                    updateTexture(&paletteTexture); updateTexture(&brightTexture);
                    reset(); texture(&paletteTexture); texture(&brightTexture);
                    auto bs=surface({255,255,255}); bs.LightTableId=lightId;
                    bs.LightInfo.light_level=255; bs.LightInfo.fade_end=31;
                    auto q=quad(128.125f);
                    polygon(&bs,q.data(),4,PF_Modulated|PF_ColorMapped|PF_NoDepthTest);
                    expect("brightmap-smooth",texelColors[index]);
                    D3D12_FinishUpdate(0);
                }
                special(HWD_SET_PALETTE_RENDERING,1);
                deleteTexture(&brightTexture);
                paletteTexel={24,180,220,255}; updateTexture(&paletteTexture); setLookup(lut.data());
            }
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
            { auto p=palettePixels(); bool untouched=true; for(auto c:p) untouched=untouched && c==0xcd;
              require("palette-off",untouched,"palette step ran with palette rendering off"); }
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
        // Water surface UV distortion and scene refraction use separate shader equations.
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
            require("water-group-counts",copiesAfter-copiesBefore==2 && tripsAfter-tripsBefore==2,"grouped water copy/roundtrip oracle failed");
            expect("water-group",{80,120,158}); D3D12_FinishUpdate(0);
            auto fog=surface({248,4,252}); fog.LightInfo.light_level=255; fog.LightInfo.fade_end=31;
            auto captureQuad=quad(128.125f);
            const FBITFIELD waterFlags=PF_Modulated|PF_ColorMapped|PF_Ripple|PF_Translucent|PF_NoDepthTest;
            const FBITFIELD fogFlags=PF_Modulated|PF_ColorMapped|PF_Fog|PF_NoTexture|PF_NoDepthTest;
            auto waterDraw=[&](UINT count=4) { texture(&water); shader(SHADER_WATER); polygon(&ws,captureQuad.data(),count,waterFlags); };
            auto fogDraw=[&](UINT count=4) { shader(SHADER_FOG); polygon(&fog,captureQuad.data(),count,fogFlags); };
            bool mixedSameList=false;
            for(int attempt=0;attempt<4 && !mixedSameList;attempt++) {
                reset({80,120,160}); auto mixedBefore=captureCounts();
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
            auto recreateBefore=captureCounts(); reset({40,80,120}); special(HWD_SET_WATER_SURFACE,2);
            waterDraw(); fogDraw(); countDelta("capture-recreate",recreateBefore,1,1);
            expect("capture-recreate",{40,80,120}); D3D12_FinishUpdate(0);
            // Actual size changes replace both capture allocations. Keep a grouped run ready across
            // each Surface call and verify that state 2 still reads the new target's known color.
            auto clip=function<void (*)(INT32,INT32,INT32,INT32,float)>("GClipRect");
            for(int size:{128,64}) {
                special(HWD_SET_WATER_SURFACE,1); waterDraw(); fogDraw();
                if(!D3D12_Surface(size,size)) throw std::runtime_error("capture resize failed");
                clip(0,0,size,size,.1f); reset({40,80,120}); special(HWD_SET_WATER_SURFACE,2);
                recreateBefore=captureCounts(); waterDraw(); fogDraw(); countDelta("capture-recreate",recreateBefore,1,1);
                same("capture-recreate","pixel after resize",size/2,size/2,probe(size/2,size/2,size),{40,80,120});
                validate(validationQueue); D3D12_FinishUpdate(0);
            }
            special(HWD_SET_WATER_SURFACE,0);

            // PreparePolygon captures these two diagonal vertices, but a two-vertex fan never
            // draws. Repeated water/fog pairs exercise coalescing of still-pending transitions.
            reset({80,120,160}); auto skippedBefore=captureCounts(); bool sameList=false;
            auto fullQuad=captureQuad; captureQuad[1]=captureQuad[2];
            for(int pair=0;pair<8;pair++) {
                UINT64 pending=Core_PendingFence(); waterDraw(2); fogDraw(2);
                if(Core_PendingFence()==pending) sameList=true;
            }
            captureQuad=fullQuad; waterDraw(); fogDraw();
            require("capture-skipped",sameList,"skipped captures did not share a pending command list");
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
                doWipe();
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
                doWipe();
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
                doWipe();
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
            reset(S); startWipe(); reset(E); endWipe(); g_wipemode=0; g_wipereverse=0; g_wipeencorewiggle=0; doWipe();
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
                same("resize-fade-map","fade map after a resize",w/2,h/2,probe(w/2,h/2,h),m40);
                quadrants(tl,tr,bl,br); makeScreen(); reset({96,96,96}); blend(PF_NoDepthTest); drawIntermission();
                const struct { int x, y; RGB want; } samples[]={{w/4,h/4,tl},{w*3/4,h/4,tr},{w/4,h*3/4,bl},{w*3/4,h*3/4,br}};
                for(auto &sample:samples) same("resize-screen","screen texture after a resize",sample.x,sample.y,probe(sample.x,sample.y,h),sample.want);
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
        reset(); renderModel(&model,0,0,0,-1,&position,2,2,0,0,&modelSurface); expectQuad("model-buffered",{192,96,48});
        reset(); renderModel(&model,0,2,1,1,&position,2,2,0,0,&modelSurface);
        // Halfway to the next frame, the quad has moved 0.6 right (x from -0.2 to 1.4), so its diagonal runs from (-0.2,-0.8) to
        // (1.4,0.8). (50,32) at NDC (0.58,-0.02) is in its upper left triangle, (56,48) at (0.77,-0.52) in the lower right one and
        // (56,15) at (0.77,0.52) in the upper left one, each well inside the quad. (14,32) is left of it.
        { auto p=pixels(); validate(validationQueue);
          const std::array<int,2> inside[]={{50,32},{56,48},{56,15}};
          for(const auto &pixel:inside) check("model-lerped",p,pixel[0],pixel[1],{192,96,48});
          check("model-lerped",p,14,32,{48,80,112}); }
        model.max_s=.5f; // UV mismatch forces the dynamic-ring fallback.
        reset(); renderModel(&model,0,0,0,-1,&position,2,2,0,0,&modelSurface); expectQuad("model-fallback",{192,96,48});
        // Every ring draw is a tile of 2x2 pixels in a 32x32 grid, with a color of its own: red and green count along the grid and each
        // frame has its own blue. A ring overwrite, stale constant or corrupt vertex of any draw, not only the last, shows in its tile.
        for(int frame=0;frame<4;frame++) {
            const auto tileColor=[frame](int tx,int ty) { return RGB{8*tx+4,8*ty+4,32+56*frame}; };
            reset();
            for(int ty=0;ty<32;ty++) for(int tx=0;tx<32;tx++) fillRect(tx/16.f-1,(tx+1)/16.f-1,1-(ty+1)/16.f,1-ty/16.f,tileColor(tx,ty));
            auto p=pixels(); validate(validationQueue);
            for(int ty=0;ty<32;ty++) for(int tx=0;tx<32;tx++) for(int dy=0;dy<2;dy++) for(int dx=0;dx<2;dx++) check("rings",p,2*tx+dx,2*ty+dy,tileColor(tx,ty));
            D3D12_FinishUpdate(0);
        }
        reset(); std::vector<FOutVector> huge(400,quad()[0]); huge[0]=quad()[0]; huge[1]=quad()[1]; huge[2]=quad()[2]; huge[3]=quad()[3]; auto hs=surface({32,220,96}); polygon(&hs,huge.data(),(FUINT)huge.size(),PF_Modulated|PF_NoTexture|PF_NoDepthTest); expect("growth",{32,220,96});
        D3D12_FinishUpdate(0); D3D12_LogPerf(1);
        if(logs.find("fallbacks 1, lerps 1")==std::string::npos) throw std::runtime_error("model buffered/lerped/fallback paths were not all exercised");
        // Every light table the palette scenes made is gone by now
        { unsigned alive=9; int parsed=0; auto pp=logs.find("UWP perf:   D3D12: palette: rendering ");
          if(pp==std::string::npos || sscanf(logs.c_str()+pp,"UWP perf:   D3D12: palette: rendering %*s %u light tables alive%n",&alive,&parsed)!=1 || !parsed) throw std::runtime_error("missing palette perf line");
          if(alive!=0) throw std::runtime_error("light tables outlived the palette scenes"); }
        unsigned vw=0,iw=0,cw=0,st=0,grows=0; auto pos=logs.find("; wraps "); if(pos==std::string::npos || sscanf(logs.c_str()+pos,"; wraps %u/%u/%u; stalls %u",&vw,&iw,&cw,&st)!=4) throw std::runtime_error("missing ring perf counters"); auto gp=logs.find("; grows ",pos); if(gp==std::string::npos || sscanf(logs.c_str()+gp,"; grows %u",&grows)!=1 || !grows || !(vw+iw+cw) || !st) throw std::runtime_error("stress did not wrap/stall and grow rings");
        unsigned ringSubmissions=0; auto rp=logs.find("mid-frame: ring "); if(rp==std::string::npos || sscanf(logs.c_str()+rp,"mid-frame: ring %u",&ringSubmissions)!=1 || !ringSubmissions) throw std::runtime_error("stress had no ring submissions");
        // Invisible geometry, such as depth-only sky walls and sprite silhouettes, never changes the target's color,
        // whichever blend modes it passes through, and always writes depth. The visible surface after it blends with
        // its own mode again.
        {
            for(const auto &from:modes) for(const auto &to:modes) {
                reset(dst);
                draw(src,from.flags|PF_Invisible|PF_Occlude,.5f,128);
                draw(src,to.flags|PF_Invisible|PF_NoTexture|PF_Occlude,.5f,128);
                expectQuad("blend-invisible",dst);
                // The depth it wrote hides a farther visible surface
                draw(src,to.flags|PF_Occlude,1.5f,128);
                expectQuad("blend-invisible-depth",dst);
                // A nearer visible surface blends over the target as its mode says
                draw(src,to.flags|PF_Occlude,.25f,128);
                expectQuad("blend-invisible-restore",blended(to.flags,src,dst,128));
            }
            D3D12_FinishUpdate(0);
        }
        // The void: Legacy GL draws the sky first, then the level, and Software draws what the level doesn't cover black.
        // D3D12_FillVoid runs after the opaque geometry and the sky claims (depth-only surfaces): pixels with depth keep
        // their color, the rest turn black, and the depth is left as it was.
        {
            const RGB sky=dst, red={200,40,20}, green={20,200,40}, blue={40,20,200}, black={0,0,0};
            auto rect=[&](float x0,float x1,RGB c,FBITFIELD flags,float z=1,int alpha=255) {
                std::vector<FOutVector> v={{x0*z,-.9f*z,z,0,0},{x1*z,-.9f*z,z,0,0},{x1*z,.9f*z,z,0,0},{x0*z,.9f*z,z,0,0}};
                auto surf=surface(c,alpha); flags|=PF_Modulated|PF_NoTexture; blend(flags); polygon(&surf,v.data(),4,flags);
            };
            // Columns 0-16 opaque geometry, 16-32 a sky claim, 32-64 nothing
            reset(sky);
            rect(-1,-.5f,red,PF_Occlude);
            rect(-.5f,0,{255,255,255},PF_Invisible|PF_Occlude);
            { auto p=pixels(); validate(validationQueue); check("void-fill",p,10,32,red); check("void-claim",p,20,32,sky); check("void-fill",p,44,32,sky); }
            D3D12_FillVoid();
            { auto p=pixels(); validate(validationQueue);
              check("void-fill",p,44,32,black); check("void-fill",p,60,32,black); check("void-fill",p,8,2,black); // above the rows the quads cover, the void is black too
              check("void-claim",p,20,32,sky); check("void-claim",p,28,32,sky); check("void-fill",p,10,32,red); }
            // Translucent geometry drawn afterwards blends with the black void, and with the sky where it was claimed
            rect(-.25f,.75f,green,PF_Translucent,.5f,128);
            { auto p=pixels(); validate(validationQueue);
              check("void-translucent",p,28,32,blended(PF_Translucent,green,sky,128));
              check("void-translucent",p,44,32,blended(PF_Translucent,green,black,128));
              check("void-translucent",p,10,32,red); check("void-translucent",p,60,32,black); }
            // The fill wrote no depth: a farther surface still shows over the void and is still hidden by the claim
            rect(-.5f,1,blue,PF_Occlude,1.5f);
            { auto p=pixels(); validate(validationQueue); check("void-depth",p,60,32,blue); check("void-depth",p,20,32,sky); check("void-depth",p,10,32,red); }
            D3D12_FinishUpdate(0);
        }
        // The fill stays inside the viewport (split-screen), though the depth clear before it covers the whole target
        {
            auto gclip=function<void (*)(INT32,INT32,INT32,INT32,float)>("GClipRect");
            reset(dst); gclip(0,0,W/2,H,.1f); D3D12_FillVoid(); gclip(0,0,W,H,.1f);
            { auto p=pixels(); validate(validationQueue); check("void-viewport",p,16,32,{0,0,0}); check("void-viewport",p,16,2,{0,0,0});
              check("void-viewport",p,48,32,dst); check("void-viewport",p,48,2,dst); }
            reset(dst); gclip(W/2,0,W,H,.1f); D3D12_FillVoid(); gclip(0,0,W,H,.1f);
            { auto p=pixels(); validate(validationQueue); check("void-viewport",p,48,32,{0,0,0}); check("void-viewport",p,16,32,dst); }
            D3D12_FinishUpdate(0);
        }
        // Inside a portal only the portal's pixels are filled: the child's far-depth reset and stencil level confine it
        {
            const RGB child={24,180,220};
            auto aperture=std::array<FOutVector,4>{{{-.6f*.25f,-.6f*.25f,.25f,0,0},{.6f*.25f,-.6f*.25f,.25f,0,0},{.6f*.25f,.6f*.25f,.25f,0,0},{-.6f*.25f,.6f*.25f,.25f,0,0}}};
            reset(dst);
            if(!D3D12_DrawPortalAperture(aperture.data(),0,false)) throw std::runtime_error("portal aperture failed");
            D3D12_ResetPortalDepth(1); D3D12_SetPortalStencil(1);
            { std::vector<FOutVector> v={{-.2f,-.9f,1,0,0},{.2f,-.9f,1,0,0},{.2f,.9f,1,0,0},{-.2f,.9f,1,0,0}};
              auto surf=surface(child); blend(PF_Modulated|PF_NoTexture|PF_Occlude); polygon(&surf,v.data(),4,PF_Modulated|PF_NoTexture|PF_Occlude); }
            D3D12_FillVoid(); D3D12_SetPortalStencil(0);
            { auto p=pixels(); validate(validationQueue);
              check("void-portal",p,32,32,child);       // the child's geometry
              check("void-portal",p,20,32,{0,0,0});     // the portal's void
              check("void-portal",p,4,32,dst);          // outside the portal, the parent's sky is untouched
              check("void-portal",p,60,32,dst); }
            D3D12_FinishUpdate(0);
        }
        // Fog and water composites write the final color themselves, so they disable blending but keep the write mask,
        // and the next surface blends normally again, however often a composite came before it.
        {
            auto shader=function<void (*)(int)>("SetShader");
            auto shaderInfo=function<void (*)(hwdshaderinfo_t,INT32)>("SetShaderInfo");
            special(HWD_SET_SHADERS,HWD_SHADEROPTION_ON);
            std::array<UINT8,4> transparent={0,0,0,1};
            GLMipmap_t waterTexture={}; waterTexture.format=GL_TEXFMT_RGBA; waterTexture.width=waterTexture.height=1; waterTexture.data=transparent.data();
            const RGB scenery={80,120,160};
            auto fogSurface=surface({248,4,252},128); fogSurface.LightInfo.light_level=255; fogSurface.LightInfo.fade_end=31;
            fogSurface.TintColor.s.red=51; fogSurface.TintColor.s.green=76; fogSurface.TintColor.s.blue=128; fogSurface.TintColor.s.alpha=10;
            auto waterSurface=surface({255,255,255}); waterSurface.LightInfo.light_level=255; waterSurface.LightInfo.fade_end=31;
            auto wide=quad(128.125f);
            const FBITFIELD waterFlags=PF_Modulated|PF_ColorMapped|PF_Ripple|PF_Translucent|PF_NoDepthTest;
            const FBITFIELD fogFlags=PF_Modulated|PF_ColorMapped|PF_Fog|PF_NoTexture|PF_NoDepthTest;
            // The fog remap's color: a tint of the scenery's brightness, as the fog-remap scene computes it
            RGB remapped={}; { const double brightness=std::sqrt(double(scenery[0]*scenery[0]+scenery[1]*scenery[1]+scenery[2]*scenery[2]));
              const double strength=std::sqrt(9.*10/255); const int tint[]={51,76,128};
              for(int c=0;c<3;c++) remapped[c]=(int)std::lround(std::clamp(brightness*tint[c]/255*strength+scenery[c]*(1-strength),0.,255.)); }
            shaderInfo(HWD_SHADERINFO_LEVELTIME,6);
            for(int use=0;use<3;use++) {
                // With the write mask off, neither composite reaches the target
                function<void (*)()>("UnSetShader")(); reset(scenery);
                texture(&waterTexture); shader(SHADER_WATER); polygon(&waterSurface,wide.data(),4,waterFlags|PF_Invisible);
                shader(SHADER_FOG); polygon(&fogSurface,wide.data(),4,fogFlags|PF_Invisible);
                expect("blend-remap-mask",scenery);
                // A fog remap draws its color, and the translucent surface after it blends over that color
                function<void (*)()>("UnSetShader")(); reset(scenery);
                shader(SHADER_FOG); polygon(&fogSurface,wide.data(),4,fogFlags);
                function<void (*)()>("UnSetShader")();
                draw(src,PF_Translucent|PF_NoDepthTest,1,128);
                expect("blend-remap-restore",blended(PF_Translucent,src,remapped,128));
            }
            deleteTexture(&waterTexture); function<void (*)()>("UnSetShader")(); special(HWD_SET_SHADERS,HWD_SHADEROPTION_OFF);
            D3D12_FinishUpdate(0);
        }
        // Add-on shaders are picked per vertex layout (polygon, sky and model), and a fragment-only replacement
        // keeps the built-in vertex stage. The native custom_shader_dispatch test counts translations and injects
        // creation failures; these scenes check what reaches the screen.
        {
            auto load=function<void (*)(int,char *,size_t,boolean)>("LoadCustomShader");
            auto compile=function<boolean (*)()>("CompileShaders");
            auto clean=function<void (*)()>("CleanShaders");
            auto shader=function<void (*)(int)>("SetShader");
            auto shaderInfo=function<void (*)(hwdshaderinfo_t,INT32)>("SetShaderInfo");
            auto skyDome=function<void (*)(gl_sky_t *)>("RenderSkyDome");
            auto setTransform=function<void (*)(FTransform *)>("SetTransform");
            // Each successful add-on compilation logs one line
            auto compilations=[&]() { size_t count=0; for(size_t at=0;(at=logs.find("compiled for polygon, sky and model layouts",at))!=std::string::npos;at++) count++; return count; };
            auto loadFragment=[&](int type,const char *code) { load(type,const_cast<char *>(code),strlen(code),true); };
            const char *const first="void main() { gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0); }";
            const char *const second="void main() { gl_FragColor = vec4(0.75, 0.5, 0.25, 1.0); }";
            const RGB firstColor={64,128,191}, secondColor={191,128,64}, builtIn={200,40,16};
            std::array<UINT8,4> texel={200,40,16,255};
            GLMipmap_t customTexture={}; customTexture.format=GL_TEXFMT_RGBA; customTexture.width=customTexture.height=1; customTexture.data=texel.data();
            // Every draw sets the identity transform first, as the engine sets its view after the sky: the sky dome
            // leaves its own matrices behind, which would put the next polygon or model off screen.
            auto drawPolygon=[&](float distance=1) {
                reset(); setTransform(&identity); texture(&customTexture); auto q=quad(distance);
                auto white=surface({255,255,255}); white.LightInfo.light_level=255; white.LightInfo.fade_end=31;
                // Colormapped, as the engine draws every shaded surface: the driver keeps the tint and fade of the last
                // colormapped surface (as r_opengl.c does), so an earlier scene's fog tint would otherwise carry over.
                const FBITFIELD flags=PF_Modulated|PF_ColorMapped|PF_NoDepthTest; blend(flags); polygon(&white,q.data(),4,flags);
            };
            auto drawSky=[&]() { reset(); texture(nullptr); setTransform(&identity); blend(PF_NoTexture|PF_NoDepthTest); skyDome(&sky); };
            auto drawModel=[&]() { reset(); setTransform(&identity); renderModel(&model,0,0,0,-1,&position,2,2,0,0,&modelSurface); };
            special(HWD_SET_SHADERS,HWD_SHADEROPTION_ON); special(HWD_SET_MODEL_LIGHTING,0);
            clean();
            // With none loaded, the built-in program draws
            const size_t none=compilations();
            if(!compile()) throw std::runtime_error("compiling without add-on shaders failed");
            shader(SHADER_FLOOR); drawPolygon(); expect("custom-none",builtIn);
            // A fragment-only replacement is drawn on every layout, with the built-in vertex stage placing the geometry
            loadFragment(SHADER_FLOOR,first);
            if(!compile() || compilations()!=none+1) throw std::runtime_error("fragment-only add-on shader did not compile");
            shader(SHADER_FLOOR);
            drawPolygon(); expect("custom-layout-poly",firstColor); expect("custom-layout-poly",{48,80,112},2,2);
            drawSky(); expect("custom-layout-sky",firstColor);
            drawModel(); expect("custom-layout-model",firstColor);
            // A bad replacement discards the working one, and a good one works again
            const char *const invalid="void main() { gl_FragColor = vec4(; }";
            loadFragment(SHADER_FLOOR,invalid);
            if(!compile() || compilations()!=none+1) throw std::runtime_error("an invalid add-on shader disabled the built-in shaders");
            shader(SHADER_FLOOR); drawPolygon(); expect("custom-replace-invalid",builtIn);
            loadFragment(SHADER_FLOOR,second);
            if(!compile() || compilations()!=none+2) throw std::runtime_error("a good add-on shader did not recover");
            drawPolygon(); expect("custom-replace-recover",secondColor);
            // Billboard clipping uses the sprite replacement
            clean(); loadFragment(SHADER_SPRITE,first);
            if(!compile()) throw std::runtime_error("sprite add-on shader did not compile");
            // Far away, as sprites are: the clipping hack pulls depth in by 80/z, which puts a quad at depth 1 behind the camera.
            shader(SHADER_SPRITECLIPHACK); drawPolygon(128.125f); expect("custom-sprite-clip",firstColor);
            // A lit model keeps its custom model shader unless a model-lighting replacement is supplied
            clean(); loadFragment(SHADER_MODEL,first);
            if(!compile()) throw std::runtime_error("model add-on shader did not compile");
            special(HWD_SET_MODEL_LIGHTING,1); shader(SHADER_MODEL); drawModel(); expect("custom-model-kept",firstColor);
            loadFragment(SHADER_MODEL_LIGHTING,second);
            if(!compile()) throw std::runtime_error("model lighting add-on shader did not compile");
            shader(SHADER_MODEL); drawModel(); expect("custom-model-lighting",secondColor);
            special(HWD_SET_MODEL_LIGHTING,0);
            // Custom water composites its own color over the refracted scenery, as the built-in water does
            clean();
            const char *const waterFragment="#version 120\nuniform vec4 poly_color;\nvoid main() {\n gl_FragColor=vec4(0.25,0.5,0.75,poly_color.a);\n}\n";
            loadFragment(SHADER_WATER,waterFragment);
            if(!compile()) throw std::runtime_error("water add-on shader did not compile");
            { const int alpha=128; const double a=alpha/255.; const float distance=128.125f;
              auto ws=surface({255,255,255},alpha); ws.LightInfo.light_level=255; ws.LightInfo.fade_end=31;
              for(int tick:{6,41}) {
                  function<void (*)()>("UnSetShader")(); reset({0,0,0});
                  for(int y=0;y<64;y++) fillRect(-1,1,1-(y+1)/32.f,1-y/32.f,{y*3,240-y*3,64});
                  texture(&customTexture); shader(SHADER_WATER); shaderInfo(HWD_SHADERINFO_LEVELTIME,tick);
                  auto wq=quad(distance);
                  polygon(&ws,wq.data(),4,PF_Modulated|PF_ColorMapped|PF_Ripple|PF_Translucent|PF_NoDepthTest);
                  auto image=pixels(); validate(validationQueue);
                  const int angle=(tick*140+(int)std::floor(distance*128))&8191;
                  const int shift=(int)std::floor(double(finesine[angle])/(4096+std::floor(distance*32)));
                  if(!shift) throw std::runtime_error("custom water refraction would not shift the scenery");
                  for(int y:{20,32,44}) {
                      const int row=std::clamp(y+shift,0,63); const RGB scenery={row*3,240-row*3,64}, water={64,128,191};
                      RGB want={}; for(int c=0;c<3;c++) want[c]=(int)std::lround(std::clamp((water[c]/255.)*a+(scenery[c]/255.)*(1-a),0.,1.)*255.);
                      // The unrefracted result differs by half a shift of three levels per row
                      check("custom-water-refraction",image,32,y,want);
                  }
                  D3D12_FinishUpdate(0);
              } }
            clean(); shader(SHADER_NONE); special(HWD_SET_SHADERS,HWD_SHADEROPTION_OFF); special(HWD_SET_MODEL_LIGHTING,0);
            deleteTexture(&customTexture); D3D12_FinishUpdate(0);
        }
        Prewarm_Stop(); if(!Core_WaitIdle(30000,"validation")) throw std::runtime_error("idle failed"); validate(queue.Get());
        D3D12_Shutdown(); validate(queue.Get());
        require("shutdown",Core_Device()==nullptr,"shutdown device lifetime oracle failed");
        if(!negative.empty()) throw std::runtime_error("unknown or unexercised negative control");
        const std::string problems=oracleProblems();
        if(!problems.empty()) throw std::runtime_error("oracles that never ran, or that aren't listed:"+problems);
        puts("Production D3D12 WARP driver scenes passed (RGB tolerance 1, GBV clean)"); return 0;
    } catch(const std::exception &e) { fprintf(stderr,"FAIL: %s\n",e.what()); D3D12_Shutdown(); if(negativeRejected) fprintf(stderr,"NEGATIVE_ORACLE_REJECTED: %s\n",negative.c_str()); return 1; }
}
