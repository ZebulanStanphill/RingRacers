// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Ronald "Eidolon" Kinard
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#include "i_video.h"

#include <algorithm>
#include <array>
#include <vector>

#include <imgui.h>
#include <tracy/tracy/Tracy.hpp>

#include "command.h"
#include "cxxutil.hpp"
#include "f_finale.h"
#include "m_fixed.h"
#include "m_misc.h"
#include "hwr2/hardware_state.hpp"
#include "hwr2/patch_atlas.hpp"
#include "hwr2/twodee.hpp"
#include "v_video.h"

// KILL THIS WHEN WE KILL OLD OGL SUPPORT PLEASE
#include "d_netcmd.h" // kill
#include "doomstat.h" // kill
#include "s_sound.h"  // kill
#include "sdl/ogl_sdl.h"
#include "st_stuff.h" // kill
#ifdef HAVE_D3D12
#include "hardware/r_d3d12/r_d3d12.h"
#include "hardware/hw_main.h"
#endif

// Legacy FinishUpdate Draws
#include "d_netcmd.h"
#ifdef HAVE_DISCORDRPC
#include "discord.h"
#endif
#include "doomstat.h"
#ifdef SRB2_CONFIG_ENABLE_WEBM_MOVIES
#include "m_avrecorder.h"
#endif
#include "st_stuff.h"
#include "s_sound.h"
#include "st_stuff.h"
#include "v_video.h"

#ifdef _UWP
#include "i_system.h"
#endif

extern "C" consvar_t cv_scr_scale, cv_scr_x, cv_scr_y;

using namespace srb2;
using namespace srb2::hwr2;
using namespace srb2::rhi;

static Rhi* g_last_known_rhi = nullptr;
static bool g_imgui_frame_active = false;
static HardwareState g_hw_state;

Handle<Rhi> srb2::sys::g_current_rhi = kNullHandle;

static bool rhi_changed(Rhi* rhi)
{
	return g_last_known_rhi != rhi;
}

static void reset_hardware_state(Rhi* rhi)
{
	// The lifetime of objects pointed to by RHI Handles is determined by the RHI itself, so it is enough to simply
	// "forget" about the resources previously known.
	g_hw_state = HardwareState {};
	g_hw_state.palette_manager = std::make_unique<PaletteManager>();
	g_hw_state.flat_manager = std::make_unique<FlatTextureManager>();
	g_hw_state.patch_atlas_cache = std::make_unique<PatchAtlasCache>(2048, 3);
	g_hw_state.twodee_renderer = std::make_unique<TwodeeRenderer>(
		g_hw_state.palette_manager.get(),
		g_hw_state.flat_manager.get(),
		g_hw_state.patch_atlas_cache.get()
	);
	g_hw_state.software_screen_renderer = std::make_unique<SoftwareScreenRenderer>();
	g_hw_state.blit_postimg_screens = std::make_unique<BlitPostimgScreens>(g_hw_state.palette_manager.get());
	g_hw_state.wipe = std::make_unique<PostprocessWipePass>();
	g_hw_state.blit_rect = std::make_unique<BlitRectPass>(BlitRectPass::BlitMode::kNearest);
	g_hw_state.sharp_bilinear_blit_rect = std::make_unique<BlitRectPass>(BlitRectPass::BlitMode::kSharpBilinear);
	g_hw_state.crt_blit_rect = std::make_unique<BlitRectPass>(BlitRectPass::BlitMode::kCrt);
	g_hw_state.crtsharp_blit_rect = std::make_unique<BlitRectPass>(BlitRectPass::BlitMode::kCrtSharp);
	g_hw_state.screen_capture = std::make_unique<ScreenshotPass>();
	g_hw_state.backbuffer = std::make_unique<UpscaleBackbuffer>();
	g_hw_state.imgui_renderer = std::make_unique<ImguiRenderer>();
	g_hw_state.wipe_frames = {};

	g_last_known_rhi = rhi;
}

static void new_twodee_frame();
static void new_imgui_frame();

static void preframe_update(Rhi& rhi)
{
	g_hw_state.palette_manager->update(rhi);
	new_twodee_frame();
	new_imgui_frame();
}

static void postframe_update(Rhi& rhi)
{
	g_hw_state.palette_manager->end_frame(rhi);
}

static void temp_legacy_finishupdate_draws()
{
	SCR_CalculateFPS();

	if (g_takemapthumbnail != TMT_NO)
	{
		return;
	}

	if (st_overlay)
	{
		if (cv_songcredits.value)
			HU_DrawSongCredits();

		if (cv_ticrate.value)
			SCR_DisplayTicRate();

		if (netgame && (consoleplayer != serverplayer || !server_lagless))
		{
			if (server_lagless)
			{
				if (consoleplayer != serverplayer)
					SCR_DisplayLocalPing();
			}
			else
			{
				for (int player = 1; player < MAXPLAYERS; player++)
				{
					if (D_IsPlayerHumanAndGaming(player))
					{
						SCR_DisplayLocalPing();
						break;
					}
				}
			}
		}
		if (cv_mindelay.value && consoleplayer == serverplayer && Playing())
			SCR_DisplayLocalPing();
#ifdef SRB2_CONFIG_ENABLE_WEBM_MOVIES
		M_AVRecorder_DrawFrameRate();
#endif
	}

	if (marathonmode)
		SCR_DisplayMarathonInfo();

	// draw captions if enabled
	if (cv_closedcaptioning.value)
		SCR_ClosedCaptions();

#ifdef HAVE_DISCORDRPC
	if (discordRequestList != NULL)
		ST_AskToJoinEnvelope();
#endif

	ST_drawDebugInfo();
}

#ifdef HWRENDER
static void finish_legacy_ogl_update()
{
	temp_legacy_finishupdate_draws();
#ifdef HAVE_D3D12
	// [UWP]: Follows the palette rendering setting, which the driver applies as it presents
	HWR_UpdatePaletteRendering();
	D3D12_FinishUpdate(cv_vidwait.value);
#else
	OglSdlFinishUpdate(cv_vidwait.value);
#endif
}
#endif

static void new_twodee_frame()
{
	g_2d = Twodee();
	Patch_ResetFreedThisFrame();
}

static void new_imgui_frame()
{
	if (g_imgui_frame_active)
	{
		ImGui::EndFrame();
		g_imgui_frame_active = false;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize.x = vid.realwidth;
	io.DisplaySize.y = vid.realheight;
	ImGui::NewFrame();
	g_imgui_frame_active = true;
}

HardwareState* sys::main_hardware_state()
{
	return &g_hw_state;
}

void I_CaptureVideoFrame()
{
	rhi::Rhi* rhi = srb2::sys::get_rhi(srb2::sys::g_current_rhi);
	hwr2::HardwareState* hw_state = srb2::sys::main_hardware_state();

	hw_state->screen_capture->set_source(static_cast<uint32_t>(vid.width), static_cast<uint32_t>(vid.height));
	hw_state->screen_capture->capture(*rhi);
}

#ifdef _UWP
// [UWP]: Log each step of the first few frames, to find where the Xbox One X hangs
extern "C" void I_OutputMsg(const char *fmt, ...);
static int uwp_traceframe = 0;
#define UWP_TRACE(step) do { if (uwp_traceframe < 3) I_OutputMsg("UWP: frame %d: %s\n", uwp_traceframe, step); } while (0)

// [UWP]: Time the parts of I_FinishUpdate for the performance summary in the log (m_perfstats.c)
extern "C" precise_t ps_uwp_twodeetime, ps_uwp_upscaletime, ps_uwp_imguitime, ps_uwp_cleanuptime, ps_uwp_presenttime,
	ps_uwp_finishtime, ps_uwp_nextframetime;
static precise_t uwp_stagestart;
#define UWP_STAGE(total) do { const precise_t now = I_GetPreciseTime(); total += now - uwp_stagestart; uwp_stagestart = now; } while (0)
#else
#define UWP_TRACE(step) do {} while (0)
#define UWP_STAGE(total) do {} while (0)
#endif

void I_StartDisplayUpdate(void)
{
	if (rendermode == render_none)
	{
		return;
	}

#ifdef HWRENDER
	if (rendermode == render_opengl)
	{
		return;
	}
#endif

	rhi::Rhi* rhi = sys::get_rhi(sys::g_current_rhi);

	if (rhi == nullptr)
	{
		// ???
		return;
	}

	UWP_TRACE("start display update");
	if (rhi_changed(rhi))
	{
		// Reset all hardware 2 state
		reset_hardware_state(rhi);
	}

	HardwareState* hw_state = &g_hw_state;

	hw_state->backbuffer->begin_pass(*rhi);

	preframe_update(*rhi);
}

void I_FinishUpdate(void)
{
#ifdef _UWP
	UWPProfPhase uwpprofphase(UWPPROF_DISPLAY);
#endif

	ZoneScoped;
	if (rendermode == render_none)
	{
		FrameMark;
		return;
	}

#ifdef HWRENDER
	if (rendermode == render_opengl)
	{
		finish_legacy_ogl_update();
		FrameMark;
		return;
	}
#endif

#ifdef _UWP
	uwp_stagestart = I_GetPreciseTime();
	I_UWPAngleMark(UWPANGLE_FINISH_START);
#endif
	temp_legacy_finishupdate_draws();

	rhi::Rhi* rhi = sys::get_rhi(sys::g_current_rhi);

	if (rhi == nullptr)
	{
		// ???
		FrameMark;
		return;
	}

	UWP_TRACE("flush 2D");
	// better hope the drawing code left the context in a render pass, I guess
	g_hw_state.twodee_renderer->flush(*rhi, g_2d);
	rhi->pop_render_pass();
	UWP_STAGE(ps_uwp_twodeetime);

	UWP_TRACE("push default render pass");
	rhi->push_default_render_pass(true);

	// Upscale draw the backbuffer (with postprocessing maybe?)
	if (cv_scr_scale.value != FRACUNIT)
	{
		float f = std::max(FixedToFloat(cv_scr_scale.value), 0.f);
		float w = vid.realwidth * f;
		float h = vid.realheight * f;
		float x = (vid.realwidth - w) * (0.5f + (FixedToFloat(cv_scr_x.value) * 0.5f));
		float y = (vid.realheight - h) * (0.5f + (FixedToFloat(cv_scr_y.value) * 0.5f));

		g_hw_state.blit_rect->set_output(x, y, w, h, true, true);
		g_hw_state.sharp_bilinear_blit_rect->set_output(x, y, w, h, true, true);
		g_hw_state.crt_blit_rect->set_output(x, y, w, h, true, true);
		g_hw_state.crtsharp_blit_rect->set_output(x, y, w, h, true, true);
	}
	else
	{
		g_hw_state.blit_rect->set_output(0, 0, vid.realwidth, vid.realheight, true, true);
		g_hw_state.sharp_bilinear_blit_rect->set_output(0, 0, vid.realwidth, vid.realheight, true, true);
		g_hw_state.crt_blit_rect->set_output(0, 0, vid.realwidth, vid.realheight, true, true);
		g_hw_state.crtsharp_blit_rect->set_output(0, 0, vid.realwidth, vid.realheight, true, true);
	}
	g_hw_state.blit_rect->set_texture(g_hw_state.backbuffer->color(), static_cast<uint32_t>(vid.width), static_cast<uint32_t>(vid.height));
	g_hw_state.sharp_bilinear_blit_rect->set_texture(g_hw_state.backbuffer->color(), static_cast<uint32_t>(vid.width), static_cast<uint32_t>(vid.height));
	g_hw_state.crt_blit_rect->set_texture(g_hw_state.backbuffer->color(), static_cast<uint32_t>(vid.width), static_cast<uint32_t>(vid.height));
	g_hw_state.crtsharp_blit_rect->set_texture(g_hw_state.backbuffer->color(), static_cast<uint32_t>(vid.width), static_cast<uint32_t>(vid.height));

	UWP_TRACE("blit backbuffer");
	switch (cv_scr_effect.value)
	{
	case 1:
		rhi->update_texture_settings(g_hw_state.backbuffer->color(), TextureWrapMode::kClamp, TextureWrapMode::kClamp, TextureFilterMode::kLinear, TextureFilterMode::kLinear);
		g_hw_state.sharp_bilinear_blit_rect->draw(*rhi);
		break;
	case 2:
		rhi->update_texture_settings(g_hw_state.backbuffer->color(), TextureWrapMode::kClamp, TextureWrapMode::kClamp, TextureFilterMode::kLinear, TextureFilterMode::kLinear);
		g_hw_state.crt_blit_rect->draw(*rhi);
		break;
	case 3:
		rhi->update_texture_settings(g_hw_state.backbuffer->color(), TextureWrapMode::kClamp, TextureWrapMode::kClamp, TextureFilterMode::kLinear, TextureFilterMode::kLinear);
		g_hw_state.crtsharp_blit_rect->draw(*rhi);
		break;
	default:
		rhi->update_texture_settings(g_hw_state.backbuffer->color(), TextureWrapMode::kClamp, TextureWrapMode::kClamp, TextureFilterMode::kNearest, TextureFilterMode::kNearest);
		g_hw_state.blit_rect->draw(*rhi);
		break;
	}
	UWP_STAGE(ps_uwp_upscaletime);

	UWP_TRACE("render imgui");
	g_hw_state.imgui_renderer->render(*rhi);

	rhi->pop_render_pass();
	UWP_STAGE(ps_uwp_imguitime);

	UWP_TRACE("postframe update");
	postframe_update(*rhi);
	UWP_STAGE(ps_uwp_cleanuptime);

	UWP_TRACE("present");
#ifdef _UWP
	I_UWPAngleMark(UWPANGLE_PRESENT_START);
#endif
	rhi->present();
#ifdef _UWP
	I_UWPAngleMark(UWPANGLE_PRESENT_END);
#endif
	UWP_STAGE(ps_uwp_presenttime);
	UWP_TRACE("finish");
	rhi->finish();
	UWP_STAGE(ps_uwp_finishtime);
	UWP_TRACE("done");
#ifdef _UWP
	uwp_traceframe++;
#endif

	FrameMark;

	// Immediately prepare to begin drawing the next frame
	I_StartDisplayUpdate();
	UWP_STAGE(ps_uwp_nextframetime);
}
