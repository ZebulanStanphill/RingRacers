# VibeRant driver thread

VibeRant D3D can record and present frames on a second CPU thread while the
game builds the next frame. It is always on. For troubleshooting, the console
command `gr_driverthread Off` (or `On`) switches it at the next
`FinishUpdate`, on Xbox and Windows desktop. The game doesn't save it, so every
launch starts with the thread on. Vulkan and Metal call the
driver on the main thread, where their presenters can use SDL.

## Command order and ownership

The main thread publishes commands to a 16 MiB byte ring. Each record starts on
a 16-byte boundary and carries its size and dispatch function. A padding record
wraps the queue. The producer waits if it runs out of room, and a single record
too large for the ring runs synchronously. Release and acquire operations
publish both the records and the space the worker has finished using. The
worker spins briefly when idle, then waits on a condition variable. Publishers
notify only when a waiter is armed.

A synchronous call waits for all published commands, then calls the same
driver implementation on the main thread. The worker can run no more driver
code until the main thread publishes another command. Only the main thread
calls the proxies. The driver functions themselves call other implementations
directly.

| Entry points | Execution with threading on |
| --- | --- |
| `DrawPolygon`, `DrawIndexedTriangles`, `Draw2DLine`, `ClearBuffer`, `SetTransform`, `SetPortalPlane`, `SetScreenPalette` | Queued with copies of pointed-to arguments. Indexed draws copy through the highest referenced index. |
| `DrawModel` | Queued with copied transform and surface, and with the scale of the model's texture coordinates and whether its VBOs match that scale, both read when the draw is queued. The worker applies the scale to the model's original coordinates, so the engine rewrites a sprite-textured model's coordinates without draining. Loaded models stay allocated, and VBO creation is synchronous. |
| `RenderSkyDome` | Copies the sky descriptor and loops for a draw from the existing GPU buffer. Rebuilds are synchronous because they read the source vertices and update the caller's descriptor. A queued draw without a GPU buffer or source data skips the draw and logs once. |
| `SetTexture` | Binds a copied name and brightmap flag, or unbinds for null. Uploads for textures without names are synchronous, as are `UpdateTexture`, `DeleteTexture`, and `ClearMipMapCache`. |
| `SetSpecialState` | Water surface, model lighting, and palette rendering queue. A changed shader option, texture filter, atlas, or anisotropy setting is synchronous. The last shader option is cached to skip redundant calls. |
| `SetBlend`, `GClipRect`, `SetShader`, `UnSetShader`, `SetShaderInfo`, `MakeScreenTexture`, `MakeScreenFinalTexture`, `DrawScreenFinalTexture`, `PostImgRedraw` | Queued. The post-image shader samples per pixel, so it needs only the copied time and effect type. |
| `SetPortalStencil`, `ResetPortalDepth`, `FillVoid`, `UseSpringMask`, `DrawPortalAperture`, `DrawSpringMask` | Queued, with copied aperture and mask vertices. An aperture reports accepted and a spring mask reports accepted for 3–32 corners. GPU failures can still prevent a draw. |
| `PortalsSupported`, `CanRollSprite`, `TakeTextureFlushRequest` | Portal support synchronizes until the driver has preflighted the pipelines, then uses the cached answer. Roll eligibility reads shader validity, the shader option, and filters, all written synchronously. A clear atomic flush request returns immediately, and a pending request synchronizes. |
| `FinishUpdate`, `LogPerf` | Queued with snapshots. Window work and effect source loading can make the frame synchronous. |
| `Init`, `Surface`, `SelectBackend`, `ReleaseWindow`, `Shutdown`, `GetPresentTrace` | Synchronous. Release and shutdown stop the worker before touching the window. `Surface` keeps its nested startup calls on the main thread. |
| `ReadRect`, `GetTextureUsed`, `CreateModelVBOs`, `CompileShaders`, `CleanShaders`, `LoadCustomShader`, `SetPalette`, `SetPaletteLookup`, `CreateLightTable`, `UpdateLightTable`, `ClearLightTables`, `ApplyFadeMap` | Synchronous. |
| `FlushScreenTextures`, `StartScreenWipe`, `EndScreenWipe`, `DoScreenWipe`, `DrawIntermissionBG`, surface probes, and test hooks | Synchronous when a probe starts or ends. From a successful `BeginSurfaceProbe` to `EndSurfaceProbe`, draws and state changes also run directly. Inactive probe hooks return immediately, including the source and texture labels called for individual draws. |

## Game symbols and thread boundaries

The game's mutable inputs are either read on the main thread and copied into
commands or accessed by synchronous calls. The lists below come from
`nm --undefined-only` on the Linux driver objects and inspection of both
D3D presenters, the D3D paths in the device core, and the UWP perf call.

| Game symbols | Access |
| --- | --- |
| `cv_reducevfx` | Copied into `DrawPolygon`, `DrawIndexedTriangles`, and `SetShader` for the water shader and refraction decisions. |
| `leveltime`, `postimgtype`, `viewcontext` | The main thread selects the current view's post-image type and captures time and type in `PostImgRedraw`. |
| `cv_scr_effect` | Captured for each frame and perf sample. Synchronous test readbacks use the current value. |
| `hwr_roll_cpu`, `hwr_roll_geometry`, `hwr_roll_shader`, `HWR_UWPTakeLazyModelLoads` | Taken and reset on the main thread before it queues a perf sample. |
| `cv_grdriverthread` | Read only by the main thread at the frame boundary. |
| `F_WipeIsToBlack`, `F_WipeIsToInvert`, `F_WipeIsToWhite`, `g_wipeencorewiggle`, `g_wipeframe`, `g_wipemode`, `g_wipereverse`, `g_wipetype`, `W_CheckNumForName`, `W_LumpLength`, `W_ReadLump` | Used by synchronous wipe calls. |
| `W_ReadShader`, `TranslateScreenEffect`, `TranslateLegacyShader` | Run during synchronous effect loading or add-on shader compilation. A worker frame can only use previously built effects. |
| `glanisotropicmode_cons_t`, `HWR_Startup` | Updated or called from synchronous `Surface`. |
| `pMasterPalette` | Read in synchronous `SetPaletteLookup`. The final palette pass uses the driver's own uploaded textures. |
| `finesine` | Read-only lookup table. |
| `I_GetPreciseTime`, `I_GetPrecisePrecision`, `I_OutputMsg`, `I_Error` | The timers use SDL's performance counter. Logging can write from either thread, with the log stream kept open until the worker stops. Worker logs skip TTF and the interactive terminal cursor. The Xbox logging timer is atomic and excludes worker time. Only the main thread calls `I_Error`. |
| `vid`, `cv_scr_scaling`, `uwp_GetScreenSize`, `UWPTraceFrameId`, `window`, presenter SDL/ANGLE calls | The main thread measures the layout and captures the trace ID. Window lookup, take, and release use SDL and ANGLE on the main thread. |
| `I_UWPProfileDriverThread` | Registers the worker with the sampler using a locked thread handle. The sampler resumes the main thread before sampling the worker. |
| `I_UWPKeepDriverThreadOffSystemCache` | Called by the worker as it starts, on its own thread. It keeps the worker on the main thread's processors, so the queue's cache lines stay in one L2 cache. |

Queued texture binding never dereferences a `GLMipmap_t`. The upload and
flush paths can retain and update that object only in synchronous calls.
The worker can set an atomic texture-flush request. Shader eligibility and
render-target dimensions only change in synchronous calls. The placement
fallback flag is atomic because a worker present can change the next frame's
layout. Model geometry stays allocated. Queued model draws read a model's
original UVs, which never change, and not the adjusted copy the main thread
rewrites. Render statistics are read and reset in command order.

## Present, pacing, and errors

Before publishing a present for frame N, the main thread waits for the worker
to finish the present for frame N−1. It can queue frame N's draws while the prior
present runs. The main thread measures the current window or screen, chooses
the layout, and records the scaling and trace information. A window take,
swap-chain resize, or desktop child-window resize makes that frame synchronous.
On desktop the wait dispatches sent window messages so DXGI can complete a
present while SDL's main thread is waiting. It leaves queued input for SDL.

The first frame that would load an effect from the WAD runs synchronously.
Later frames use the cached result, including a failed build. A worker frame
runs palette postprocessing, the chosen effect, placement, submission, and
present using the snapshot and the driver's resources.

After a worker fatal, the queue drops pending commands and the next proxy call
raises the error on the main thread. Shutdown gives the worker two seconds to
drain. On a timeout, it logs the failure and leaves the live graphics objects
for process teardown so shutdown cannot destroy resources under a stuck call.

## Measurements and coverage

The `VibeRant perf: driver thread:` line reports worker command time, main
thread waiting for the previous present and for queue space, bytes queued per
frame, and drain counts and wait time by reason. Worker time includes waits in
the present and excludes idle queue waits. On Xbox the sampling profiler
records the worker during play under **VibeRant D3D: driver thread**.
`uwp/tools/profile-report.py` lists that phase separately from main-thread samples.

The WARP harness runs the pixel scenes both ways and checks copied inputs,
queue wrap, present pacing, and synchronization counts. Its headless presenter
exercises GPU submission, and a separate Windows executable checks desktop
window presentation. Xbox behavior, real-GPU timing, and performance gains
require hardware measurements.
