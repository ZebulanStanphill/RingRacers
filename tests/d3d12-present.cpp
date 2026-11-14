// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
// Runs the production desktop presenter (d3d12_present_win32.cpp) and the production device core on WARP, with the debug
// layer on, against a real top-level window that is never shown. Only the engine's services and SDL's window lookup are
// stubs. The driver's PlaceFrame is replaced by a stub that records what the presenter asks of it and clears the buffer.
#include "hardware/r_d3d12/d3d12_core.h"
#include "hardware/r_d3d12/d3d12_present.h"
#include "doomdef.h"
#include "command.h"
#include "i_system.h"
#include "screen.h"
#include "SDL.h"
#include "SDL_syswm.h"
#include <d3d12sdklayers.h>
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

static std::string logs;
struct SDL_Window { HWND hwnd; }; // what the stubbed SDL window lookup hands to the presenter
extern "C" {
SDL_Window *window = nullptr;
consvar_t cv_scr_scaling{};
precise_t I_GetPreciseTime() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
UINT64 I_GetPrecisePrecision() { LARGE_INTEGER n; QueryPerformanceFrequency(&n); return n.QuadPart; }
void I_OutputMsg(const char *fmt, ...) { char b[8192]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap); logs += b; fputs(b,stdout); }
void I_Error(const char *fmt, ...) { char b[2048]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap); throw std::runtime_error(std::string("I_Error: ")+b); }
const char *SDLCALL SDL_GetError(void) { return "stubbed SDL"; }
SDL_bool SDLCALL SDL_GetWindowWMInfo(SDL_Window *w, SDL_SysWMinfo *info) {
    if(!w || !info) return SDL_FALSE;
    info->subsystem=SDL_SYSWM_WINDOWS;
    info->info.win.window=w->hwnd;
    return SDL_TRUE;
}
}

// ==========================================================================
//                                                                 CHECKING
// ==========================================================================

static void check(bool ok,const char *what) {
    if(!ok) throw std::runtime_error(std::string("check failed: ")+what);
    printf("ok: %s\n",what);
}
static unsigned occurrences(const char *what) { unsigned n=0; for(size_t at=0;(at=logs.find(what,at))!=std::string::npos;at+=strlen(what)) n++; return n; }

static ID3D12InfoQueue *validationQueue=nullptr;
// Any debug-layer warning or error, or any Core_Errors report, fails the run. The sole allowlist is the advisory about a clear
// value that doesn't match the one the buffer was created with, which affects only how fast the clear is (the placement stub
// clears the swap chain's buffers as the driver does).
static void validate(const char *when) {
    bool failed=false;
    for(UINT64 i=0;i<validationQueue->GetNumStoredMessages();i++) {
        SIZE_T bytes=0;
        if(FAILED(validationQueue->GetMessage(i,nullptr,&bytes))) throw std::runtime_error("GetMessage size failed");
        std::vector<UINT8> data(bytes);
        auto m=reinterpret_cast<D3D12_MESSAGE *>(data.data());
        if(FAILED(validationQueue->GetMessage(i,m,&bytes))) throw std::runtime_error("GetMessage failed");
        if(m->Severity==D3D12_MESSAGE_SEVERITY_WARNING && m->ID==D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE) continue;
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_WARNING) { fprintf(stderr,"D3D12 validation #%d: %s\n",m->ID,m->pDescription); failed=true; }
    }
    validationQueue->ClearStoredMessages();
    if(failed) throw std::runtime_error(std::string("debug layer message after ")+when);
    if(Core_Errors()) throw std::runtime_error(std::string("Core_Errors nonzero after ")+when);
    printf("ok: no debug layer messages after %s\n",when);
}

// ==========================================================================
//                                              THE WINDOW AND THE FRAME SOURCE
// ==========================================================================

static HWND parentWindow=nullptr;
static LRESULT CALLBACK ParentProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) { return DefWindowProcW(hwnd,message,wparam,lparam); }
// A top-level popup window, never shown, whose client area is its whole size
static HWND makeParent(int width,int height) {
    WNDCLASSEXW wc={}; wc.cbSize=sizeof wc; wc.lpfnWndProc=ParentProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"RingRacersPresentTestParent";
    if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("RegisterClassEx failed");
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"d3d12 present test",WS_POPUP,0,0,width,height,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hwnd) throw std::runtime_error("CreateWindowEx failed");
    return hwnd;
}
static void resizeParent(int width,int height) {
    if(!SetWindowPos(parentWindow,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)) throw std::runtime_error("SetWindowPos failed");
}
static void clientSize(HWND hwnd,UINT &width,UINT &height) {
    RECT r={}; GetClientRect(hwnd,&r); width=(UINT)(r.right-r.left); height=(UINT)(r.bottom-r.top);
}

static constexpr D3D12_RESOURCE_STATES kSourceState=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
static ComPtr<ID3D12Resource> makeSource(UINT width,UINT height) {
    D3D12_HEAP_PROPERTIES heap={}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc={}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=width; desc.Height=height; desc.DepthOrArraySize=1; desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> source;
    if(Core_Failed(Core_Device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,kSourceState,nullptr,IID_PPV_ARGS(&source)),"CreateCommittedResource (frame source)"))
        throw std::runtime_error("couldn't make a frame source");
    return source;
}

// What the presenter asked of the driver's PlaceFrame, which this stands in for: it checks the target and clears it
struct PlaceCall {
    int calls=0;
    screen_scaling::Placement placement;
    UINT width=0,height=0;
    bool targetMatches=false; // the target resource is the swap chain's buffer, of the size the presenter said, in the swap chain's format
    D3D12_RESOURCE_STATES sourceState=D3D12_RESOURCE_STATE_COMMON;
};
static PlaceCall placeCall;
static bool placeSucceeds=true;
static bool PlaceStub(ID3D12Resource *,D3D12_RESOURCE_STATES sourceState,ID3D12Resource *target,D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    const screen_scaling::Placement &placement,UINT targetWidth,UINT targetHeight) {
    placeCall.calls++; placeCall.placement=placement; placeCall.width=targetWidth; placeCall.height=targetHeight; placeCall.sourceState=sourceState;
    const D3D12_RESOURCE_DESC desc=target->GetDesc();
    placeCall.targetMatches=desc.Width==targetWidth && desc.Height==targetHeight && desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM;
    if(!placeSucceeds) return false; // records nothing, as the contract says
    const float color[4]={0.1f,0.2f,0.4f,1.0f};
    Core_List()->ClearRenderTargetView(rtv,color,0,nullptr);
    return true;
}

// The window's child: there is exactly one while the window is taken, covering the parent's client area, and it takes no input
static HWND checkChild(const char *when) {
    const HWND child=GetWindow(parentWindow,GW_CHILD);
    check(child!=nullptr,(std::string("the parent has a child window ")+when).c_str());
    check(GetWindow(child,GW_HWNDNEXT)==nullptr,"it is the only child");
    check(GetParent(child)==parentWindow,"its parent is the game window");
    const LONG_PTR style=GetWindowLongPtrW(child,GWL_STYLE), exStyle=GetWindowLongPtrW(child,GWL_EXSTYLE);
    check((style&WS_CHILD) && (style&WS_VISIBLE) && (style&WS_DISABLED),"it is a visible, disabled child window");
    check((exStyle&WS_EX_NOACTIVATE)!=0,"it is never activated");
    check(SendMessageW(child,WM_NCHITTEST,0,MAKELPARAM(5,5))==HTTRANSPARENT,"hit testing passes through it");
    check(SendMessageW(child,WM_ERASEBKGND,0,0)==1,"it doesn't erase its background");
    UINT pw,ph,cw,ch; clientSize(parentWindow,pw,ph); clientSize(child,cw,ch);
    check(pw==cw && ph==ch,"it covers the parent's whole client area");
    RECT r={}; GetWindowRect(child,&r); POINT origin={0,0}; ClientToScreen(parentWindow,&origin);
    check(r.left==origin.x && r.top==origin.y,"at the parent's client origin");
    return child;
}

// Presents frames of a source, with and without vsync, at one of Screen Scaling's modes (0 Fit, 1 Integer, 2 1:1), and checks what the
// presenter did: the rectangle and k it asked the driver to place the frame at, which each case works out by hand, the swap chain's
// size, which is the window's, and the child window.
static void showFrames(const char *what,ID3D12Resource *source,UINT frameWidth,UINT frameHeight,int mode,int x,int y,int w,int h,int k) {
    cv_scr_scaling.value=mode;
    UINT clientWidth,clientHeight; clientSize(parentWindow,clientWidth,clientHeight);
    for(int frame=0;frame<2;frame++) {
        placeCall=PlaceCall(); placeSucceeds=true;
        Present_Frame(source,kSourceState,frameWidth,frameHeight,frame==1,PlaceStub);
        check(placeCall.calls==1,(std::string(what)+": the driver placed the frame once").c_str());
        const screen_scaling::Placement &p=placeCall.placement;
        if(!(!p.fit && p.x==x && p.y==y && p.w==w && p.h==h && p.k==k))
            fprintf(stderr,"%s: placed fit %d (%d, %d) %dx%d k=%d, expected (%d, %d) %dx%d k=%d\n",what,p.fit,p.x,p.y,p.w,p.h,p.k,x,y,w,h,k);
        check(!p.fit && p.x==x && p.y==y && p.w==w && p.h==h && p.k==k,(std::string(what)+": the placement is the one worked out by hand").c_str());
        check(placeCall.width==clientWidth && placeCall.height==clientHeight && placeCall.targetMatches,(std::string(what)+": the target is a buffer of the window's size").c_str());
        check(placeCall.sourceState==kSourceState,(std::string(what)+": the source is left to the driver in its state").c_str());
        INT32 copied,shown; UINT32 copyFrame,showFrame; Present_GetTrace(&copied,&shown,&copyFrame,&showFrame);
        check(copied>=0 && copied==shown,(std::string(what)+": a buffer was presented").c_str());
    }
    check(Present_SwapChainBytes()==3ull*clientWidth*clientHeight*4,(std::string(what)+": the swap chain is the window's size").c_str());
    checkChild(what);
    validate(what);
}

int main() {
    ComPtr<ID3D12Debug> debug;
    if(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { fputs("D3D12 debug layer unavailable; install Windows Graphics Tools\n",stderr); return 77; }
    debug->EnableDebugLayer();
    puts("D3D12 debug layer enabled");
    try {
        Core_ForceWarp(true);
        if(!Core_Init()) throw std::runtime_error("Core_Init failed");
        ComPtr<ID3D12InfoQueue> queue; if(FAILED(Core_Device()->QueryInterface(IID_PPV_ARGS(&queue)))) throw std::runtime_error("InfoQueue unavailable");
        validationQueue=queue.Get();

        parentWindow=makeParent(320,200);
        SDL_Window sdlWindow={parentWindow}; window=&sdlWindow;
        UINT cw,ch; clientSize(parentWindow,cw,ch);
        check(cw==320 && ch==200 && !IsWindowVisible(parentWindow),"the test window is hidden and its client area is 320x200");
        ComPtr<ID3D12Resource> frame=makeSource(64,48), large=makeSource(640,480);

        // Taking: the child and the swap chain, which is the client area's size
        check(GetWindow(parentWindow,GW_CHILD)==nullptr,"the window has no child before taking it");
        check(Present_SwapChainBytes()==0,"there is no swap chain before taking the window");
        check(Present_TakeWindow(),"the window is taken");
        const HWND firstChild=checkChild("after taking it");
        check(Present_SwapChainBytes()==3ull*320*200*4,"the swap chain is the client area's size");
        check(Present_TakeWindow() && GetWindow(parentWindow,GW_CHILD)==firstChild,"taking it again changes nothing");
        validate("taking the window");

        // A 64x48 frame in a 320x200 window. Fit: it fills the height, 200 * 64 / 48 is 266 (rounded down), (320 - 266) / 2 is 27, with
        // linear filtering (k 0). Integer: 320 / 64 is 5 and 200 / 48 is 4, so 4 times: 256x192 at (32, 4). 1:1: 64x48 at (128, 76).
        showFrames("Fit at 320x200",frame.Get(),64,48,0,27,0,266,200,0);
        showFrames("Integer at 320x200",frame.Get(),64,48,1,32,4,256,192,4);
        showFrames("1:1 at 320x200",frame.Get(),64,48,2,128,76,64,48,1);
        // A frame larger than the window isn't placed by Integer or 1:1, so it is fit: 640x480 in 320x200 is the same 266x200 rectangle
        showFrames("Integer with a larger frame",large.Get(),640,480,1,27,0,266,200,0);
        showFrames("1:1 with a larger frame",large.Get(),640,480,2,27,0,266,200,0);

        // The window is resized: the child and the swap chain follow, with the next frame. 500x300: Fit 300 * 64 / 48 is 400, at (50, 0).
        // Integer: 500 / 64 is 7 and 300 / 48 is 6, so 6 times: 384x288 at (58, 6). 1:1: 64x48 at (218, 126).
        resizeParent(500,300);
        showFrames("Fit at 500x300",frame.Get(),64,48,0,50,0,400,300,0);
        showFrames("Integer at 500x300",frame.Get(),64,48,1,58,6,384,288,6);
        showFrames("1:1 at 500x300",frame.Get(),64,48,2,218,126,64,48,1);
        check(occurrences("resized the swap chain from 320x200 to 500x300")==1,"the swap chain was resized once, to the new client size");

        // No client area (minimized): the frame's commands are submitted, nothing is presented or placed, and nothing is lost
        resizeParent(0,0);
        placeCall=PlaceCall(); placeSucceeds=true;
        Present_Frame(frame.Get(),kSourceState,64,48,false,PlaceStub);
        Present_Frame(frame.Get(),kSourceState,64,48,true,PlaceStub);
        { INT32 copied,shown; UINT32 copyFrame,showFrame; Present_GetTrace(&copied,&shown,&copyFrame,&showFrame);
          check(placeCall.calls==0 && copied==-1 && shown==-1,"a window with no client area presents nothing"); }
        check(Present_SwapChainBytes()==3ull*500*300*4,"and keeps its swap chain");
        validate("a frame with no client area");
        // and when it has one again, smaller, the swap chain follows. 160x100: Fit 100 * 64 / 48 is 133, at (13, 0). Integer: 2 times, 128x96 at (16, 2).
        resizeParent(160,100);
        showFrames("Fit at 160x100",frame.Get(),64,48,0,13,0,133,100,0);
        showFrames("Integer at 160x100",frame.Get(),64,48,1,16,2,128,96,2);
        check(occurrences("resized the swap chain from 500x300 to 160x100")==1,"the swap chain was resized to the new client size");

        // Releasing: the child is destroyed and the swap chain is gone; the parent is as it was
        const HWND secondChild=GetWindow(parentWindow,GW_CHILD);
        Present_ReleaseWindow();
        check(!IsWindow(secondChild) && GetWindow(parentWindow,GW_CHILD)==nullptr,"releasing destroys the child window");
        check(Present_SwapChainBytes()==0,"and the swap chain");
        Present_ReleaseWindow();
        check(GetWindow(parentWindow,GW_CHILD)==nullptr,"releasing again does nothing");
        validate("releasing the window");

        // A second take works, at the window's size by then (160x100), and a frame that can't be placed (the driver returns false) turns
        // black for that frame, after which frames are copied into a swap chain of their size, which is what the window stretches
        check(Present_TakeWindow(),"the window is taken again");
        checkChild("after taking it again");
        check(Present_SwapChainBytes()==3ull*160*100*4,"the swap chain is the client area's size again");
        showFrames("Fit after taking it again",frame.Get(),64,48,0,13,0,133,100,0);
        resizeParent(320,200);
        showFrames("Fit at 320x200 again",frame.Get(),64,48,0,27,0,266,200,0);
        placeCall=PlaceCall(); placeSucceeds=false; cv_scr_scaling.value=1;
        Present_Frame(frame.Get(),kSourceState,64,48,false,PlaceStub);
        check(placeCall.calls==1 && occurrences("couldn't place the frame in the swap chain")==1,"a frame the driver can't place is logged");
        validate("a frame that couldn't be placed");
        for(int i=0;i<2;i++) {
            placeCall=PlaceCall(); placeSucceeds=true;
            Present_Frame(frame.Get(),kSourceState,64,48,i==1,PlaceStub);
            check(placeCall.calls==0,"frames are no longer placed");
        }
        check(Present_SwapChainBytes()==3ull*64*48*4,"the swap chain is the frame's size, which DXGI stretches");
        checkChild("while copying");
        resizeParent(500,300);
        Present_Frame(frame.Get(),kSourceState,64,48,false,PlaceStub);
        UINT w500,h500; clientSize(parentWindow,w500,h500);
        check(Present_SwapChainBytes()==3ull*64*48*4 && w500==500 && h500==300,"the swap chain stays the frame's size as the window changes");
        checkChild("while copying at 500x300");
        validate("copying frames");

        // Taking the window again tries placing again; without a placement function, frames are copied, from the first
        Present_ReleaseWindow();
        check(Present_TakeWindow(),"the window is taken a third time");
        check(Present_SwapChainBytes()==3ull*500*300*4,"at the window's size");
        showFrames("Fit after the third take",frame.Get(),64,48,0,50,0,400,300,0);
        placeCall=PlaceCall();
        Present_Frame(frame.Get(),kSourceState,64,48,false,nullptr);
        check(occurrences("couldn't place the frame in the swap chain")==2,"a missing placement function is the same failure");
        Present_Frame(frame.Get(),kSourceState,64,48,true,nullptr);
        check(Present_SwapChainBytes()==3ull*64*48*4,"and frames are copied");
        validate("frames without a placement function");

        // Presenting didn't fail, and the log has what happened
        Present_LogPerf(30);
        check(logs.find("; 0 present failures")!=std::string::npos && logs.find("IDXGISwapChain::Present failed")==std::string::npos,"no present failed");
        check(logs.find("created the window to present on")!=std::string::npos,"the log says the window was created");

        // Shutting down: the swap chain and the child go, and nothing is left for the debug layer to report
        if(!Core_WaitIdle(30000,"validation")) throw std::runtime_error("idle failed");
        const HWND thirdChild=GetWindow(parentWindow,GW_CHILD);
        Present_Shutdown(true);
        check(!IsWindow(thirdChild) && GetWindow(parentWindow,GW_CHILD)==nullptr && Present_SwapChainBytes()==0,"shutting down releases the swap chain and the child window");
        validate("shutting down the presenter");
        validationQueue=nullptr; queue.Reset();
        frame.Reset(); large.Reset();
        Core_Shutdown(true);
        check(Core_Device()==nullptr,"the device is gone");
        DestroyWindow(parentWindow);
        puts("Desktop D3D12 presenter checks passed (WARP, debug layer)");
        return 0;
    } catch(const std::exception &e) {
        fprintf(stderr,"FAIL: %s\n",e.what());
        return 1;
    }
}
