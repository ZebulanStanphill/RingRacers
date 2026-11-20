#!/usr/bin/env python3
"""Generate small native harnesses from the actual patched engine sources."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from source import Source
from title_fixture import generate_bsp, generate_fixture
from title_replay import generate_title


def render(template, parts, output):
    original = template.read_text(encoding="utf-8")
    text = original
    tokens = set(re.findall(r"@@(\w+)@@", original))
    if tokens != set(parts):
        raise ValueError(f"{template}: template tokens and supplied code differ")
    for name, implementation in parts.items():
        # Restore harness locations after the production source's #line directive.
        token = f"@@{name}@@"
        next_line = original.count("\n", 0, original.index(token)) + 2
        text = text.replace(
            token, implementation + f"\n#line {next_line} {json.dumps(template.as_posix())}"
        )
    output.write_text(text, encoding="utf-8")


def function_section(source, name, first, after):
    """Extract a bounded part of one function, retaining production line numbers."""
    code = source.function(name).split("\n", 1)[1].rstrip("\n")
    if code.count(first) != 1:
        raise ValueError(f"{source.path}: {name}: ambiguous or missing start {first!r}")
    start = code.index(first)
    end = code.index(after, start + len(first))
    offset = source.text.index(code)
    return source.annotated(offset + start, offset + end)



def generate_pathfind(engine, templates, output):
    def implementation(source, first):
        start = source.text.index(first)
        code = source.annotated(start, len(source.text))
        # Adapt C's allocation casts and zero initializers for the C++ harness.
        for lhs, typ in [
            ("path->array", "pathfindnode_t *"),
            ("nodesarray", "pathfindnode_t *"),
            ("nodesarrayrealloc", "pathfindnode_t *"),
            ("closedset", "pathfindnode_t **"),
            ("pathfindscratch.nodes", "pathfindnode_t *"),
            ("pathfindscratch.closed", "UINT8 *"),
            ("pathfindscratch.closedset", "pathfindnode_t **"),
            ("pathfindscratch.lookup", "size_t *"),
            ("heap->array", "bheapitem_t *"),
        ]:
            code = re.sub(rf"(?<![\w.])({re.escape(lhs)}\s*=\s*)(Z_(?:Malloc|Calloc|Realloc)\()",
                          rf"\1({typ})\2", code)
        return code.replace("= {0};", "= {};")

    first_capacity = "static const size_t DEFAULT_NODEARRAY_CAPACITY"
    astar = implementation(Source(engine / "src/k_pathfind.c"), first_capacity)
    mark_closed = "pathfindscratch.closed[currentnode - nodesarray] = true;"
    clear_lookup = (
        "pathfindscratch.lookupcapacity = capacity;\n"
        "\t}\n"
        "\tmemset(pathfindscratch.lookup, 0, capacity * sizeof(*pathfindscratch.lookup));"
    )
    rebase = "nodesarrayrealloc[j].camefrom = &nodesarrayrealloc[arrayindex];"
    mutations = {
        "NO_CLOSED_FLAG": (mark_closed, mark_closed.replace("true", "false")),
        "STALE_LOOKUP": (clear_lookup, (
            "pathfindscratch.lookupcapacity = capacity;\n"
            "\t\tmemset(pathfindscratch.lookup, 0, capacity * sizeof(*pathfindscratch.lookup));\n"
            "\t}"
        )),
        "WRONG_REBASE": (rebase, rebase.replace("[arrayindex]", "[0]")),
    }
    for name, (original, _) in mutations.items():
        if astar.count(original) != 1:
            raise ValueError(f"Could not inject the A* {name} negative control")

    render(templates / "pathfind.cpp.in", {
        "HEAP": implementation(Source(engine / "src/k_bheap.c"),
                               "static boolean K_BHeapItemValidate(bheap_t *heap, bheapitem_t *item)\n{"),
        "ASTAR": astar,
        "REFERENCE": implementation(Source(templates.parent / "fixtures/pathfind-baseline.c"), first_capacity),
        **{name: astar.replace(original, replacement) for name, (original, replacement) in mutations.items()},
    }, output / "pathfind.cpp")


def generate(engine, output):
    output.mkdir(parents=True, exist_ok=True)
    templates = Path(__file__).parent / "templates"
    generate_fixture(templates.parent / "fixtures", output)
    generate_title(engine, output, templates, render, function_section)
    generate_pathfind(engine, templates, output)
    render(templates / "split_projection.cpp.in", {
        "TRANSFORM": Source(engine / "src/hardware/r_viberant/r_viberant.cpp").function("VibeRant_SetTransform"),
        "FRUSTUM": Source(engine / "src/hardware/hw_clip.c").section("#define RMUL", "// SRB2CB I don't think"),
    }, output / "split_projection.cpp")
    render(templates / "software_shear.cpp.in", {
        "AIMING": Source(engine / "src/hardware/hw_main.c").function("HWR_SetTransformAiming"),
        "TRANSFORM": Source(engine / "src/hardware/r_viberant/r_viberant.cpp").function("VibeRant_SetTransform"),
        "FREELOOK": Source(engine / "src/r_fps.cpp").function("R_SetupFreelook"),
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "FIXED_DIV": Source(engine / "src/m_fixed.c").function("FixedDiv2"),
    }, output / "software_shear.cpp")
    render(templates / "plane_polygon_loops.cpp.in", {
        "POLYGONS": Source(engine / "src/hardware/hw_bsp.c").section(
            "extrasubsector_t *extrasubsectors = NULL;", "\n#endif //HWRENDER"),
    }, output / "plane_polygon_loops.cpp")
    maputl = Source(engine / "src/p_maputl.c")
    game_map = Source(engine / "src/p_map.c")
    thing_guards = function_section(game_map, "PIT_CheckThing",
                                    "\t// don't clip against self", "\tif (thing->flags & MF_PAPERCOLLISION)")
    prefilter = game_map.function("P_ThingSurelyOutOfReach")
    respawn_guard = "\tif (P_MobjIsReappearing(thing))\n\t\treturn BMIT_CONTINUE;"
    touching_edge = ") >= blockdist"
    sum_range = "blockdist > INT32_MAX || blockdist < INT32_MIN\n\t\t|| "
    if (thing_guards.count(respawn_guard) != 1 or prefilter.count(touching_edge) != 2
            or prefilter.count(sum_range) != 1):
        raise ValueError("Could not inject the thing check order negative controls")
    render(templates / "thing_check_order.cpp.in", {
        "PREFILTER": prefilter,
        "PRODUCTION": thing_guards,
        "REFERENCE": (templates.parent / "fixtures/thing-check-order-baseline.inc").read_text(encoding="utf-8"),
        "NO_RESPAWN": thing_guards.replace(respawn_guard, "\n"),
        "STRICT_PREFILTER": prefilter.replace("P_ThingSurelyOutOfReach", "StrictPrefilter")
            .replace(touching_edge, ") > blockdist"),
        "UNCHECKED_RADII": prefilter.replace("P_ThingSurelyOutOfReach", "UncheckedRadiiPrefilter")
            .replace(sum_range, "\n\t\t"),
    }, output / "thing_check_order.cpp")
    thing_walk = game_map.function("P_BlockThingsIteratorCheck")
    opening = function_section(game_map, "PIT_CheckThing", "\tif (g_tm.thing == NULL", "\t// Ignore spectators")
    if opening.count("return BMIT_STOP;") != 1 or opening.count("return BMIT_CONTINUE;") != 3:
        raise ValueError("PIT_CheckThing's opening tests are not the ones the thing walk's skip test mirrors")
    skip_mover = "g_tm.thing != NULL && !P_MobjWasRemoved(g_tm.thing)"
    skip_removed_next = ("\t\t\tif (next && P_MobjWasRemoved(next))\n\t\t\t{\n\t\t\t\tP_SetTarget(&bnext, NULL);\n"
                         "\t\t\t\treturn true;\n\t\t\t}\n\n")
    release_end = "\tP_SetTarget(&bnext, NULL); // Release what an earlier called thing left held when only skipped things follow it.\n"
    release_skip = "\t\t\t\tP_SetTarget(&bnext, NULL);\n\t\t\t\treturn true;"
    for part in (skip_mover, skip_removed_next, release_end, release_skip):
        if thing_walk.count(part) != 1:
            raise ValueError("Could not inject the thing walk negative controls: " + part)
    def thing_walk_control(name, old, new):
        return thing_walk.replace("P_BlockThingsIteratorCheck", name).replace(old, new)
    render(templates / "thing_walk.cpp.in", {
        "TYPES": Source(engine / "src/p_maputl.h").enum("BlockItReturn_t"),
        "PREFILTER": prefilter,
        "OPENING": opening,
        "REFERENCE": (templates.parent / "fixtures/thing-walk-baseline.c").read_text(encoding="utf-8"),
        "PRODUCTION": thing_walk,
        "NO_MOVER_TEST": thing_walk_control("NoMoverTestWalk", skip_mover, "true"),
        "NO_REMOVED_NEXT": thing_walk_control("NoRemovedNextWalk", skip_removed_next, ""),
        "NO_RELEASE_AT_END": thing_walk_control("NoReleaseAtEndWalk", release_end, ""),
        "NO_RELEASE_ON_REMOVED_NEXT": thing_walk_control("NoReleaseOnRemovedNextWalk", release_skip, "\t\t\t\treturn true;"),
    }, output / "thing_walk.cpp")
    polyobj = Source(engine / "src/p_polyobj.c")
    line_box = maputl.function("P_SkipLineBox")
    line_walk = maputl.function("P_BlockLinesIteratorInternal")
    box_walk = maputl.function("P_BlockLinesIteratorBox")
    edge_test = "bbox[BOXRIGHT] <= linebox[BOXLEFT]"
    if line_box.count(edge_test) != 1:
        raise ValueError("Could not inject the blockmap edge-comparison negative control")
    mover_test = "(!mover || *mover) && "
    if line_box.count(mover_test) != 1:
        raise ValueError("Could not inject the blockmap missing-mover negative control")
    mover_read = "g_tm.thing->z + g_tm.thing->height"
    line_state = Source(engine / "src/r_state.h")
    block_lines = {
        "TYPES": line_state.section(
            "typedef struct\n{\n\tfixed_t bbox[4];\n\tsize_t validcount;", "\nextern linecheck_t *linechecks;")
            + Source(engine / "src/p_maputl.h").enum("BlockItReturn_t"),
        "REFERENCE": (templates.parent / "fixtures/block-lines-baseline.c").read_text(encoding="utf-8"),
        "ACCESSOR": line_state.function("P_LineBox"),
        "MOVE": polyobj.function("Polyobj_bboxAdd") + polyobj.function("Polyobj_bboxSub"),
        "WALKS": line_box + line_walk + box_walk + maputl.function("P_BlockLinesIterator"),
        "STRICT_WALK": line_box.replace(edge_test, "bbox[BOXRIGHT] < linebox[BOXLEFT]")
            .replace("P_SkipLineBox", "StrictSkipLineBox")
            + line_walk.replace("P_SkipLineBox", "StrictSkipLineBox")
            .replace("P_BlockLinesIteratorInternal", "StrictLineWalk")
            + box_walk.replace("P_BlockLinesIteratorInternal", "StrictLineWalk")
            .replace("P_BlockLinesIteratorBox", "StrictBoxWalk"),
        "UNGUARDED_WALK": line_box.replace(mover_test, "((void)mover, true) && ")
            .replace("P_SkipLineBox", "UnguardedSkipLineBox")
            + line_walk.replace("P_SkipLineBox", "UnguardedSkipLineBox")
            .replace("P_BlockLinesIteratorInternal", "UnguardedLineWalk")
            + box_walk.replace("P_BlockLinesIteratorInternal", "UnguardedLineWalk")
            .replace("P_BlockLinesIteratorBox", "UnguardedBoxWalk"),
    }
    # The test supplies its own query box and mover, so it can't see what the game's three walks pass. Each must
    # pass the box its callback tests, and only PIT_CheckLine, which reads through g_tm.thing first, a mover.
    for call in ("P_BlockLinesIteratorBox(bx, by, g_tm.bbox, &g_tm.thing, PIT_CheckLine);",
                 "P_BlockLinesIteratorBox(bx, by, g_tm.bbox, NULL, PIT_GetSectors);",
                 "P_BlockLinesIteratorBox(bx, by, g_tm.precipbbox, NULL, PIT_GetPrecipSectors);"):
        if game_map.text.count(call) != 1:
            raise ValueError("p_map.c no longer has exactly one call: " + call)
    if game_map.text.count("P_BlockLinesIteratorBox(") != 3:
        raise ValueError("p_map.c has a P_BlockLinesIteratorBox call that block_lines doesn't know")
    for callback in ("CheckLine", "GetSectors", "GetPrecipSectors"):
        prefix = game_map.function("PIT_" + callback).split("\n\tif (P_BoxOnLineSide", 1)[0]
        if callback == "CheckLine":
            # The test stands in for the fault of reading through a cleared g_tm.thing.
            if prefix.count(mover_read) != 1:
                raise ValueError("PIT_CheckLine no longer reads g_tm.thing before its box test as the test expects")
            prefix = prefix.replace(mover_read, "MoverTop(ld)")
            prefix += "\n\t(void)thingtop;\n\t(void)open;"
        block_lines[callback.upper()] = prefix + "\n\treturn Record(ld);\n}\n"
    render(templates / "block_lines.cpp.in", block_lines, output / "block_lines.cpp")
    events = Source(engine / "src/d_main.cpp")
    render(templates / "event_queue.cpp.in", {
        "ADDED": events.function("HandleGamepadDeviceAdded"),
        "REMOVED": events.function("HandleGamepadDeviceRemoved"),
        "DEVICE": events.function("HandleGamepadDeviceEvents"),
        "POST": events.function("D_PostEvent"),
    }, output / "event_queue.cpp")
    upright = Source(engine / "src/hardware/hw_main.c")
    render(templates / "upright_wall.cpp.in", {
        "CLIP": upright.function("HWR_ClipWallToUpright"),
        "CALL": upright.section("memcpy(uprightwall, wallVerts, sizeof uprightwall);",
            "\t\t\t// I don't think that solid walls can use translucent linedef types..."),
        "RESTORE": upright.section("memcpy(wallVerts, uprightwall, sizeof uprightwall);", "\n\t\t}\n"),
    }, output / "upright_wall.cpp")
    render(templates / "quit_signal.cpp.in", {
        "HANDLER": Source(engine / "src/sdl/i_system.cpp").section(
            "static volatile sig_atomic_t g_quit_signal", "\n#ifdef HAVE_TERMIOS"),
    }, output / "quit_signal.cpp")
    video_menu = Source(engine / "src/menus/options-video-1.c")
    render(templates / "video_fullscreen.cpp.in", {
        "QUERY": Source(engine / "src/sdl/i_video.cpp").function("I_VideoAlwaysFullscreen"),
        "HELPER": video_menu.function("M_RemoveVideoCvar"),
        "REFRESH": video_menu.function("M_RefreshVideoOptions"),
        "OPEN": function_section(Source(engine / "src/menus/options-1.c"), "M_InitOptions",
            "\t(void)choice;", "\n\tOPTIONS_MainDef.menuitems"),
        "MENU": video_menu.section("// options menu\nmenuitem_t OPTIONS_Video[] =", "\n};\n\nmenu_t") + "};\n"
            + video_menu.section("menu_t OPTIONS_VideoDef = {", "\n};\n") + "};\n",
    }, output / "video_fullscreen.cpp")
    render(templates / "renderer_setting.cpp.in", {
        "CHANGE": Source(engine / "src/screen.c").function("SCR_ChangeRenderer"),
        "DEFAULT": Source(engine / "src/cvars.cpp").section(
            "#if defined(HAVE_VIBERANT) && defined(_UWP) // [UWP]: Default", "consvar_t cv_parallelsoftware"),
        "VALUES": Source(engine / "src/screen.c").section("CV_PossibleValue_t cv_renderer_t[]", "// ========================================================================="),
        "STARTUP": Source(engine / "src/sdl/i_video.cpp").section("\t// Renderer choices", "\tborderlesswindow ="),
    }, output / "renderer_setting.cpp")
    render(templates / "renderer_description.cpp.in", {
        "ENUM": Source(engine / "src/k_menu.h").enum("vaopt_e"),
        "REFRESH": Source(engine / "src/menus/options-video-advanced.c").function("M_RefreshAdvancedVideoOptions"),
    }, output / "renderer_description.cpp")
    render(templates / "overlay_blend.cpp.in", {
        "BLEND": Source(engine / "src/hardware/hw_main.c").section("FBITFIELD HWR_GetBlendModeFlag(INT32 ast)", "FBITFIELD HWR_TranstableToAlpha"),
    }, output / "overlay_blend.cpp")
    fade = Source(engine / "src/hardware/hw_main.c")
    render(templates / "sprite_fade.cpp.in", {
        "HELPER": fade.function("HWR_FadeSplitSprite"),
        "OVERRIDE": fade.function("HWR_OverrideObjectLightLevel"),
        "FULLBRIGHT": Source(engine / "src/r_things.cpp").function("R_ThingIsFullBright"),
        "SEMIBRIGHT": Source(engine / "src/r_things.cpp").function("R_ThingIsSemiBright"),
        "FULLDARK": Source(engine / "src/r_things.cpp").function("R_ThingIsFullDark"),
        "PAPER": Source(engine / "src/r_things.cpp").function("R_ThingIsPaperSprite"),
        "LOOP": function_section(fade, "HWR_SplitSprite",
            "\t\t// even if we aren't changing colormap", "\n\t\tif (i + 1 < sector->numlights)"),
    }, output / "sprite_fade.cpp")
    render(templates / "sprite_height.cpp.in", {
        "HEIGHT": function_section(fade, "HWR_SplitSprite", "\trealtop = top = baseWallVerts[3].y;",
            "\n\tendrealtop = endtop = baseWallVerts[2].y;").replace("return;", "return false;"),
    }, output / "sprite_height.cpp")
    shadow_hardware = Source(engine / "src/hardware/hw_main.c")
    shadow_software = Source(engine / "src/r_things.cpp")
    render(templates / "shadow_draw.cpp.in", {
        "PAPER": shadow_software.function("R_ThingIsPaperSprite"),
        "FLOOR": shadow_software.function("R_ThingIsFloorSprite"),
        "SCALE": shadow_hardware.function("HWR_ShadowDrawScale"),
        "POSITION": shadow_hardware.function("HWR_ShadowDrawPosition"),
        "SHEAR": shadow_hardware.function("HWR_ShadowDrawShear"),
        "FIXED_DIV": Source(engine / "src/m_fixed.c").function("FixedDiv2"),
        "SOFTWARE_SKEW": shadow_software.function("R_SkewShadowSprite"),
        "CALL": shadow_hardware.section("\tshadowdraw = HWR_ShadowDrawScale(thing, &interp, tz,", "\n\n\tthis_xscale"),
        "SOFTWARE_WIDTH": shadow_software.section("\toffset2 = FixedMul(spr_width, FixedMul(spritexscale, this_scale));", "\n\n\tif (papersprite)"),
        "SOFTWARE_STEP": shadow_software.section("\t\tiscale = (patch->width<<FRACBITS)/(x2-x1+1);", "\n\t\tx1 +="),
        "SOFTWARE_SCALE": shadow_software.section(
            "\t// Check if this sprite needs to be rendered like a shadow", "\tif (!shadowskew)"),
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
    }, output / "shadow_draw.cpp")
    shadow_cache = Source(engine / "src/hardware/hw_cache.c")
    shadow_parts = {
        "COLOR_MAP": shadow_hardware.function("HWR_ShadowColorMap").replace("= Z_Malloc(", "= (UINT8 *)Z_Malloc("),
        "BIND": function_section(shadow_hardware, "HWR_DrawDropShadow",
            "\tif (!(gpatch &&", "\n\tscalemul ="),
        "LIGHT_BLEND": function_section(shadow_hardware, "HWR_DrawDropShadow",
            "\tshadowVerts[0].t =", "\n\tif (HWR_UseShader())"),
        "SOFTWARE_SELECT": function_section(shadow_software, "R_ProjectDropShadow",
            "\tshadow->transmap =", "\n\tobjectsdrawn++;"),
        "SOFTWARE_COLUMN": Source(engine / "src/r_draw_column.cpp").function("R_DrawDropShadowColumn"),
        "TEXTURE_PALETTE": shadow_cache.function("HWR_GetTexturePalette"),
        "DRAW_COLUMN": shadow_cache.function("HWR_DrawColumnInCache"),
    }
    for token, name in (("GET_PATCH", "HWR_GetPatch"), ("GET_MAPPED_PATCH", "HWR_GetMappedPatch"),
                        ("LOAD_PATCH", "HWR_LoadPatchMipmap"), ("UPDATE_PATCH", "HWR_UpdatePatchMipmap")):
        shadow_parts[token] = shadow_cache.function(name).replace(
            "grPatch = patch->hardware;", "grPatch = (GLPatch_t *)patch->hardware;").replace(
            "newMipmap = calloc(", "newMipmap = (GLMipmap_t *)calloc(").replace(
            "newMipmap->colormap = Z_Calloc(", "newMipmap->colormap = (GLColormap_t *)Z_Calloc(")
    render(templates / "drop_shadow_color.cpp.in", shadow_parts, output / "drop_shadow_color.cpp")
    finale = Source(engine / "src/f_finale.c")
    render(templates / "intro.cpp.in", {
        "TIMES": finale.section("#define NUMINTROSCENES", "// custom intros"),
        "DISCLAIMER_STATE": finale.enum("disclaimerstate"),
        "TICKER": finale.function("F_IntroTicker"),
        "RESPONDER": finale.function("F_IntroResponder"),
    }, output / "intro.cpp")
    sdlvideo = Source(engine / "src/sdl/i_video.cpp")
    render(templates / "startup_gamepads.cpp.in", {
        "ADDED": sdlvideo.function("Impl_HandleControllerDeviceAddedEvent"),
        "OPEN": sdlvideo.function("I_OpenStartupGamepads"),
    }, output / "startup_gamepads.cpp")
    render(templates / "frame_schedule.cpp.in", {}, output / "frame_schedule.cpp")
    tcp = Source(engine / "src/i_tcp.c")
    errors = Source(engine / "src/i_tcp_detail.h")
    render(templates / "udp_send.cpp.in", {
        "WINSOCK_ERRORS": errors.section("\t#undef errno", "\t#ifndef IOC_VENDOR"),
        "SEND_TO_ADDR": tcp.function("SOCK_SendToAddr"),
        "SEND": tcp.function("SOCK_Send"),
    }, output / "udp_send.cpp")

    network = Source(engine / "src/d_clisrv.c")
    game = Source(engine / "src/g_game.c")
    render(templates / "online.cpp.in", {
        "CLIENT_MODE": network.enum("cl_mode_t"),
        "KEEPALIVE": network.function("CL_SendClientKeepAlive"),
        "EXPAND_TICS": network.function("ExpandTics"),
        "TITLE_CARD": game.function("G_PreLevelTitleCard"),
        "PING_UPDATE": network.function("PingUpdate"),
    }, output / "online.cpp")

    menu = Source(engine / "src/menus/options-1.c")
    render(templates / "menu.cpp.in", {
        "MENU_STATE": menu.section("struct optionsmenu_s optionsmenu;", "void M_ResetOptions(void)"),
        "RESET_OPTIONS": menu.function("M_ResetOptions"),
        "CHANGE_COLOR": menu.function("M_OptionsChangeBGColour"),
        "QUIT_OPTIONS": menu.function("M_OptionsQuit"),
        "TICK_OPTIONS": menu.function("M_OptionsTick"),
    }, output / "menu.cpp")

    main = Source(engine / "src/d_main.cpp")
    render(templates / "trace.cpp.in", {
        "TRACE_IMPLEMENTATION": main.section("// Main-thread diagnostics only.", "\n#endif\n\nstatic bool D_Display"),
        "FRAME_GATING": main.section("\tUWPTraceBeginFrame();", "\n#endif"),
    }, output / "trace.cpp")

    video = Source(engine / "src/v_video.cpp")
    draw = Source(engine / "src/hardware/hw_draw.c")
    render(templates / "fills.cpp.in", {
        "HUD_ALPHA_TABLES": video.section("static UINT8 hudplusalpha[11]", "static const UINT8 *v_colormap"),
        "GL_ALPHA_TABLE": draw.section("static UINT8 softwaretranstogl[11]", "static UINT8 softwaretranstogl_hi"),
        "HUD_TRANSLUCENCY": video.function("V_GetHUDTranslucency"),
        "SNAP": video.function("V_AdjustXYWithSnap"),
        "ALPHA_LEVEL": video.function("V_GetAlphaLevel"),
        "HARDWARE_FILL": draw.function("HWR_DrawFill"),
        "FILL": video.function("V_DrawFill"),
    }, output / "fills.cpp")

    cache = Source(engine / "src/hardware/hw_cache.c")
    render(templates / "pictures.cpp.in", {
        "PICTURE_TABLES": cache.section("// Source pixels and GPU pixels", "static void HWR_DrawPicInCache"),
        "CONVERT": cache.function("HWR_DrawPicInCache"),
        "GET_PIC": cache.function("HWR_GetPic"),
    }, output / "pictures.cpp")

    driver12 = Source(engine / "src/hardware/r_viberant/r_viberant.cpp")
    captures = Source(engine / "src/hardware/r_viberant/viberant_captures.inc")
    hardware = Source(engine / "src/hardware/hw_main.c")
    subprocess.run([sys.executable, str(Path(__file__).parent / "addons/shader-smoke/build.py"),
        "--check", "--header", str(output / "smoke_pk3.h")], check=True)
    render(templates / "custom_pk3.cpp.in", {
        "TARGETS": hardware.section("customshaderxlat_t shaderxlat[]", "void HWR_LoadAllCustomShaders"),
        "FIND_DEFS": hardware.function("HWR_FindShaderDefs"),
        "LOAD_SHADERS": hardware.function("HWR_LoadCustomShadersFromFile"),
    }, output / "custom_pk3.cpp")
    pipeline_header = Source(engine / "src/hardware/r_viberant/viberant_pipeline.h")
    render(templates / "custom_dispatch.cpp.in", {
        "PIPELINE_ENUMS": pipeline_header.section("enum VSKind", "D3D12_BLEND_DESC MakeBlendDesc"),
        "CUSTOM_STATE": driver12.section("struct CustomProgram\n{", "void SetBlendMode(FBITFIELD flags)"),
        "PROGRAM_EXISTS": driver12.function("ProgramExists"),
        "RETIRE": driver12.function("RetireCustomPipelines"),
        "ADVANCE": driver12.function("AdvanceCustomGeneration"),
        "VALIDATION_KEY": driver12.function("CustomValidationKey"),
        "COMPILE": driver12.function("VibeRant_CompileShaders"),
        "CLEAN": driver12.function("VibeRant_CleanShaders"),
        "LOAD": driver12.function("VibeRant_LoadCustomShader"),
        "SET_SHADER": driver12.function("VibeRant_SetShader").replace("bool reduceEffects", "bool reduceEffects = cv_reducevfx.value != 0"),
    }, output / "custom_dispatch.cpp")
    fog = Source(engine / "src/hardware/r_viberant/shaders.hlsl")
    renderer = Source(engine / "src/r_main.cpp")
    fog_shader = fog.function("PSFogRemap").replace(" : SV_Target", "")
    fog_shader = fog_shader.replace("int3(", "makeint3(").replace("float4(", "make4(")
    fog_shader = re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", fog_shader)
    render(templates / "fog.cpp.in", {
        "FOG_SHADER": fog_shader,
        "FIXED_DIV": Source(engine / "src/m_fixed.c").function("FixedDiv2"),
        "FOG_ROW": re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", fog.function("FogColormap")),
        "SOFTWARE_Z": renderer.function("R_InitLightTables"),
        "SOFTWARE_SCALE": renderer.section("\tmemset(scalelight,", "\n\t// continue to do the software"),
    }, output / "fog.cpp")

    render(templates / "surface_light.cpp.in", {
        "FIXED_DIV": Source(engine / "src/m_fixed.c").function("FixedDiv2"),
        "FOG_ROW": re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", fog.function("FogColormap")),
        "SURFACE_ROW": re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", fog.function("R_DoomColormap")),
        "SOFTWARE_Z": renderer.function("R_InitLightTables"),
        "SOFTWARE_SCALE": renderer.section("\tmemset(scalelight,", "\n\t// continue to do the software"),
        "LIGHT_CONSTANTS": driver12.section("\tc.lightPlane = !(g_program", "\tconst bool updateView ="),
        "PROGRAM_UNIFORMS": driver12.section("struct ProgramUniforms\n{", "ProgramUniforms g_uniforms"),
        "UNIFORM_LIGHT": driver12.section("\tif (surface != NULL)\n\t{\n\t\tu.lighting =", "\n\tif (directional)"),
        # Software's scale index for a sprite, with its semibright statement
        "PROJECT_INDEX": Source(engine / "src/r_things.cpp").section(
            "\t\tlindex = FixedMul(xscale, LIGHTRESOLUTIONFIX)>>(LIGHTSCALESHIFT);", "\n\n\t\tvis->colormap = lights_array[lindex];"),
        "SPLIT_INDEX": Source(engine / "src/r_things.cpp").section(
            "\t\t\t\tlindex = FixedMul(sprite->xscale, LIGHTRESOLUTIONFIX)>>(LIGHTSCALESHIFT);", "\n\n\t\t\t\tnewsprite->colormap = spritelights[lindex];"),
    }, output / "surface_light.cpp")

    semibright_hw = Source(engine / "src/hardware/hw_main.c")
    render(templates / "semibright_light.cpp.in", {
        "SEMIBRIGHT": Source(engine / "src/r_things.cpp").function("R_ThingIsSemiBright"),
        "USE_SHADER": semibright_hw.function("HWR_UseShader"),
        "SEMIBRIGHT_LIGHT": semibright_hw.function("HWR_SemiBrightLight"),
        "POST_HEAD": function_section(semibright_hw, "HWR_ObjectLightLevelPost", "\tconst boolean semibright =", "\tconst boolean papersprite"),
        "POST_TAIL": function_section(semibright_hw, "HWR_ObjectLightLevelPost", "\t// [ZE]: Only an approximation of Software", "\n}"),
        "SPLIT_SPRITE": semibright_hw.function("HWR_SplitSprite"),
        "DRAW_SPRITE": semibright_hw.function("HWR_DrawSprite"),
        "DRAW_MODEL": Source(engine / "src/hardware/hw_md2.c").function("HWR_DrawModel"),
        "DRAW_PRECIPITATION": semibright_hw.function("HWR_DrawPrecipitationSprite"),
        "DRAW_DROP_SHADOW": semibright_hw.function("HWR_DrawDropShadow"),
    }, output / "semibright_light.cpp")

    for kind, param, srv in (("Fog", 16, 11), ("Water", 18, 12)):
        name = "Capture" + kind + "Scene"
        # Expected constants and boxes are literal (see the tables in capture_viberant.cpp.in), not recomputed.
        expected_constants = ("WaterBytes(p.waterViewport[0],p.waterViewport[1],p.waterViewport[2],"
            "p.waterViewport[3],c.waterMode)" if kind == "Water" else "FogBytes(c.fogPlane,30,.5f)")
        bind_checks = (f"assert(bound[{srv}]==&Scene&&bound[13]==&g_waterSine);" if kind == "Water"
            else f"assert(bound[{srv}]==&Scene);")
        clear_binds = (f"bound[{srv}]=bound[13]=nullptr;" if kind == "Water" else f"bound[{srv}]=nullptr;")
        split = f"""
        for(const FlagCase &c:flagCases){{
            {'SetWaterSurface(1);' if kind == 'Water' else ''}
            size_t before=copies.size();int rounds=begins;
            {clear_binds}constants[{param}].clear();
            assert(Capture(&surface,c.flags,poly,4));
            assert(copies.size()==before+1&&begins==rounds+1&&ends==begins);
            SameBox(copies.back(),p.centered);
            assert(constants[{param}]=={expected_constants});
            {bind_checks}
        }}
"""
        specific = """
    // Each face gets current scenery: captures of an identical target copy again.
    size_t count=copies.size();
    for(int i=0;i<2;++i)assert(Capture(&surface,PF_Fog,poly,4));
    assert(copies.size()==count+2);
    // The surface light already includes darkness; only quantize and clamp it.
    // The view ratio is the viewport width over the 128 pixel screen.
    struct LightCase { int light, lightnum; };
    struct WidthCase { int width; float ratio; };
    for(const LightCase &c:std::vector<LightCase>{{0,0},{8,1},{16,2},{24,3},{240,30},{255,31},{400,31}})
        for(const WidthCase &w:std::vector<WidthCase>{{128,1.f},{64,.5f},{32,.25f}}){
            g_gl.viewport[2]=w.width;surface.LightInfo.light_level=c.light;constants[16].clear();
            assert(Capture(&surface,PF_Fog,poly,4));
            assert(constants[16]==FogBytes(0,c.lightnum,w.ratio));
        }
    g_gl.viewport[2]=128;surface.LightInfo.light_level=240;
"""
        if kind == "Water":
            specific = """
    // The viewport rectangle in the constants is {left, top, right, bottom}, inclusive of the last row and column.
    SetWaterSurface(1);constants[18].clear();
    assert(Capture(&surface,PF_Translucent,poly,4));assert(constants[18]==WaterBytes(0,0,127,95,0));
    g_gl.viewport[0]=32;g_gl.viewport[1]=16;g_gl.viewport[2]=96;g_gl.viewport[3]=64;
    SetWaterSurface(1);constants[18].clear();
    assert(Capture(&surface,PF_Environment,poly,4));assert(constants[18]==WaterBytes(32,16,127,79,5));
    g_gl.viewport[0]=g_gl.viewport[1]=0;g_gl.viewport[2]=128;g_gl.viewport[3]=96;
    // Four-sided growth issues up to four strips in one transition round trip.
    const std::vector<std::vector<int>> rectangles={{52,40,60,48},{48,38,58,46},
        {57,43,66,53},{50,25,62,42},{44,48,64,61},{40,20,70,67}};
    for(int reverse:{0,1}){
        InitTarget();const auto original=g_rt.pixels;std::vector<bool> copied(128*96,false);
        SetWaterSurface(1);UINT64 pixelsBefore=g_stats.waterPixels;bool sawFour=false;
        int ux0=128,uy0=96,ux1=0,uy1=0;
        for(size_t i=0;i<rectangles.size();++i){
            const auto &r=rectangles[reverse?rectangles.size()-1-i:i];
            auto piece=Rectangle(r[0],r[1],r[2],r[3]);if(i)SetWaterSurface(2);
            size_t before=copies.size();int rounds=begins;
            bound[12]=bound[13]=nullptr;
            assert(Capture(&surface,PF_Translucent,piece.data(),4));
            assert(bound[12]==&Scene&&bound[13]==&g_waterSine);
            size_t strips=copies.size()-before;assert(strips<=4);sawFour|=strips==4;
            assert(begins-rounds==int(strips!=0)&&ends==begins);
            for(size_t n=before;n<copies.size();++n){
                auto b=copies[n];for(UINT y=b.top;y<b.bottom;++y)for(UINT x=b.left;x<b.right;++x){
                    assert(!copied[y*128+x]);copied[y*128+x]=true;
                }
            }
            for(size_t n=0;n<copied.size();++n)if(copied[n])assert(Scene.pixels[n]==original[n]);
            // The strips together cover exactly the bounding box of every piece so far: each rectangle
            // plus a 16 row ripple margin above and below (clamped to the 96 row target).
            ux0=std::min(ux0,r[0]);ux1=std::max(ux1,r[2]);
            uy0=std::min(uy0,std::max(r[1]-16,0));uy1=std::max(uy1,std::min(r[3]+16,96));
            for(int y=0;y<96;++y)for(int x=0;x<128;++x)
                assert(copied[y*128+x]==(x>=ux0&&x<ux1&&y>=uy0&&y<uy1));
            before=copies.size();rounds=begins;
            assert(Capture(&surface,PF_Translucent,piece.data(),4));
            assert(copies.size()==before&&begins==rounds);
            for(int y=r[1];y<r[3];++y)for(int x=r[0];x<r[2];++x)g_rt.pixels[y*128+x]=99999;
        }
        assert(g_stats.waterPixels-pixelsBefore==UINT64(std::count(copied.begin(),copied.end(),true)));
        if(!reverse)assert(sawFour);
    }
    // Identical bounds in a new group/view or standalone draw capture again.
    for(int state:{0,1}){
        SetWaterSurface(state);size_t before=copies.size();
        assert(Capture(&surface,PF_Translucent,poly,4));assert(copies.size()==before+1);
    }
    SetWaterSurface(1);size_t fullBefore=copies.size();
    assert(Capture(&surface,PF_Translucent,poly,4,true));
    assert(copies.size()==fullBefore+1);D3D12_BOX full={0,0,0,128,96,1};SameBox(copies.back(),full);
"""
        render(templates / "capture_viberant.cpp.in", {
            "MULTIPLY": driver12.function("Multiply"),
            "GLOBALS": (driver12.section("GpuTexture g_waterScene, g_waterSine, g_fogScene;", "bool BindCaptureState();") if kind == "Water" else
                driver12.section("GpuTexture g_waterScene, g_waterSine, g_fogScene;", "UploadAllocation g_waterConstants") +
                driver12.section("UINT64 g_testWaterCopies", "UINT8 g_waterConstantData")),
            "BOUNDS": captures.function(kind + "CaptureBounds").replace(kind + "CaptureBounds", "CaptureBounds"),
            "CAPTURE": captures.function(name).replace(name, "Capture").replace(kind + "CaptureBounds", "CaptureBounds"),
            "CAPTURE_KIND": "#define RR_TEST_WATER_CAPTURE" if kind == "Water" else "",
            "SPLIT_TEST": split,
            "SPECIFIC_TEST": specific,
            "WATER_GROUPING": (templates / "water_grouping.cpp.in").read_text() if kind == "Water" else "",
            "CONTINUES": hardware.function("HWR_WaterPlaneContinues") if kind == "Water" else "",
            "DISPATCH": hardware.section("\t\t\tconst planeinfo_t *plane = sortnode[sortindex[i]].plane;", "\n\t\t}\n#endif") if kind == "Water" else "",
            "SINE_FAILURE": "failSine=true;assert(!Capture(&surface,PF_Translucent,poly,4));failSine=false;" if kind == "Water" else "",
        }, output / (kind.lower() + "_capture_viberant.cpp"))
        generated = output / (kind.lower() + "_capture_viberant.cpp")
        generated.write_text(re.sub(r"\bScene\b", "g_" + kind.lower() + "Scene", generated.read_text()))

    slopes = Source(engine / "src/p_slopes.c")
    render(templates / "wall_light.cpp.in", {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "SLOPE_HEIGHT": slopes.function("P_GetSlopeZAt"),
        "LIGHT_HEIGHT": slopes.function("P_GetLightZAt"),
        "FOF_BOTTOM": slopes.function("P_GetFFloorBottomZAt"),
        "STEP_LIGHT": hardware.function("HWR_StepWallLight"),
        "SKIPS_OFFSET": hardware.function("HWR_SoftwareSkipsWallOffset"),
        "SLICE_LIGHT": hardware.function("HWR_SliceWallLight"),
        "CLIP_SLICE": hardware.function("HWR_DrawWallLightSlice"),
        "SPLIT_WALL": hardware.function("HWR_TranslucentWallLight") + hardware.function("HWR_SplitWall"),
    }, output / "wall_light.cpp")
    segs = Source(engine / "src/r_segs.cpp")
    process = hardware.function("HWR_ProcessSeg").split("\n", 1)[1]
    # The two side branches deliberately share spelling. Bound each by its own
    # position in this function, preserving source locations for diagnostics.
    side_start = "\t\t\t\t\t\tif (blendmode != PF_Masked)"
    # The side seen from the front sector takes the fog colormap's light (HWR_FogColormapWallLight), the side seen
    # from the back sector does not; each section ends with its own masked call's statement
    side_end = "HWR_ProjectWall(wallVerts, &Surf, PF_Masked,"
    side_parts = []
    offset = hardware.text.index(process.rstrip("\n"))
    start = 0
    for _ in range(2):
        start = process.index(side_start, start)
        end = process.index(";\n", process.index(side_end, start)) + 1
        side_parts.append(hardware.annotated(offset + start, offset + end))
        start = end
    render(templates / "translucent_wall_light.cpp.in", {
        "HELPER": hardware.function("HWR_TranslucentWallLight"),
        "FOG_LIGHT": hardware.function("HWR_FogColormapWallLight"),
        "SKIPS_OFFSET": hardware.function("HWR_SoftwareSkipsWallOffset"),
        "SPLIT": function_section(hardware, "HWR_SplitWall",
            "\t\tif (!(list[i].flags & FOF_NOSHADE))", "\n\t\tsolid = false;"),
        "MID": function_section(hardware, "HWR_ProcessSeg",
            "\t\t\telse if (!(blendmode & PF_Masked))", "\n\t\t}\n\n\t\t// Sky culling"),
        "SIDE_FRONT": side_parts[0], "SIDE_BACK": side_parts[1],
        "SW_MID": function_section(segs, "R_RenderMaskedSegLoop",
            "\t\tif (tripwire)", "\n\t\tif (lightnum < 0)"),
        "SW_LIST": function_section(segs, "R_RenderMaskedSegLoop",
            "\t\t\tif ((R_CheckColumnFunc(COLDRAWFUNC_FUZZY) == false)\n\t\t\t\t|| (rlight->flags & FOF_FOG)",
            "\n\t\t\trlight->lightnum = lightnum;"),
        "SW_SIDE": function_section(segs, "R_RenderThickSideRange",
            "\t\t// Get correct light level!", "\n\t\tif (lightnum < 0)"),
    }, output / "translucent_wall_light.cpp")
    user = Source(engine / "src/p_user.c")
    render(templates / "first_person_angle.cpp.in", {
        "HELPER": user.function("P_FirstPersonFollowKart"),
        "SLOPE": user.section("static angle_t firstpersonpitch[MAXSPLITSCREENPLAYERS];",
            "boolean P_MoveChaseCamera(player_t *player, camera_t *thiscam, boolean resetcalled)"),
        "AIM": Source(engine / "src/r_main.cpp").function("R_SetupAimingFrame"),
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "TICK": Source(engine / "src/p_tick.c").function("P_RunChaseCameras"),
    }, output / "first_person_angle.cpp")
    main = Source(engine / "src/r_main.cpp")
    render(templates / "first_person_title.cpp.in", {
        "HELPER": main.function("R_ViewNeedsCamera"),
        "CHOICE": function_section(main, "R_SetupFrame",
            "\tif (player->spectator)", "\n\tnewview->sky = false;"),
    }, output / "first_person_title.cpp")
    render(templates / "camera_position.cpp.in", {
        "CAMERA_MENU": Source(engine / "src/menus/options-video-camera.c").section(
            "// The rows show cv_dummycameraposition", "\tNULL,\n\tNULL,\n};") + "\tNULL,\n\tNULL,\n};\n",
        "VIDEO_MENU": Source(engine / "src/menus/options-video-1.c").section(
            "// options menu\nmenuitem_t OPTIONS_Video[] =", "\n};\n\nmenu_t") + "};\n"
            + Source(engine / "src/menus/options-video-1.c").section("menu_t OPTIONS_VideoDef = {", "\n};\n") + "};\n",
        "CVARS": Source(engine / "src/cvars.cpp").section(
            "consvar_t cv_chasecam[MAXSPLITSCREENPLAYERS] = {", "\nconsvar_t cv_deadzone"),
    }, output / "camera_position.cpp")
    things = Source(engine / "src/r_things.cpp")
    batching = Source(engine / "src/hardware/hw_batching.c")
    batch_helper = batching.function("HWR_SetBatchTextures")
    batch_code = batching.section("// The texture for the next polygon", "\n\n#endif // HWRENDER")
    # C allocation conversions only; all collection, sorting and draw logic stays production code.
    for lhs, typ in [("finalVertexArray", "FOutVector"), ("finalVertexIndexArray", "UINT32"),
                     ("polygonArray", "PolygonArrayEntry"), ("polygonIndexArray", "UINT32"),
                     ("unsortedVertexArray", "FOutVector"), ("polygonSortKeys", "PolygonSortKey"),
                     ("new_index_array", "unsigned int")]:
        batch_code = batch_code.replace(lhs + " = malloc(", lhs + " = (" + typ + " *)malloc(")
    group_sort = batching.section("// [ZE]: Each group keeps its first polygon",
                                  "// [ZE]: Fills the sort key")
    for lhs, typ in [("polygonGroupSlots", "UINT32"), ("polygonSortGroups", "PolygonSortGroup"),
                     ("polygonGroupOrder", "UINT32"), ("polygonGroupForIndex", "UINT32")]:
        assignment = lhs + " = malloc("
        cast = lhs + " = (" + typ + " *)malloc("
        batch_code = batch_code.replace(assignment, cast)
        group_sort = group_sort.replace(assignment, cast)
    # Remove lightTableId from both hash and equality so these keys always share a group.
    light_table_fields = ["\tMIX_GROUP_FIELD(key->lightTableId);\n",
                          "\n\t\t&& key1->lightTableId == key2->lightTableId"]
    bad_group_sort = group_sort
    for field in light_table_fields:
        if bad_group_sort.count(field) != 1:
            raise ValueError("Could not inject the batch sort light table negative control")
        bad_group_sort = bad_group_sort.replace(field, "")
    render(templates / "batch_sort_groups.cpp.in", {
        "KEY": batching.section("// [ZE]: What the sort compares", "// [ZE]: One compact key"),
        "COMPARE": batching.section("// [ZE]: Compares one field", "// [ZE]: Each group keeps"),
        "SORT": group_sort,
        "BAD_SORT": bad_group_sort,
        "BASELINE": Source(templates.parent / "fixtures/batch-sort-baseline.c").section(
            "// Frozen index sort", "\n// END"),
    }, output / "batch_sort_groups.cpp")
    # The three local new_array declarations have distinct pointer types.
    batch_code = batch_code.replace("new_array = malloc(polygon", "new_array = (PolygonArrayEntry *)malloc(polygon")
    batch_code = batch_code.replace("new_array = malloc(", "new_array = (FOutVector *)malloc(")
    cache_code = "\n".join(cache.function(n) for n in ["HWR_LoadPatchMipmap", "HWR_GetPatch", "HWR_GetSpriteBrightmap", "HWR_GetTexture", "HWR_GetLevelFlat"])
    cache_code = cache_code.replace("grPatch = patch->hardware;", "grPatch = (GLPatch_t *)patch->hardware;")
    cache_code = cache_code.replace("patch_t *patch = W_CachePatchNum", "patch_t *patch = (patch_t *)W_CachePatchNum")
    render(templates / "shared_brightmap.cpp.in", {"BATCHING": batch_code, "CACHE": cache_code}, output / "shared_brightmap.cpp")
    opengl = Source(engine / "src/hardware/r_opengl/r_opengl.c")
    # Normalize only the exported macro signatures so Source.function can parse them.
    opengl.text = re.sub(r"EXPORT void HWRAPI\((\w+)\) ", r"void \1", opengl.text)
    from source import syntax
    opengl.code = syntax(opengl.text)
    gl_code = "\n".join(opengl.function(n) for n in ["SetNoTexture", "UpdateTexture", "SetTexture"])
    gl_code = gl_code.replace("FTextureInfo *newTex = calloc(", "FTextureInfo *newTex = (FTextureInfo *)calloc(")
    constants = sorted(set(re.findall(r"\bGL_[A-Z0-9_]+", gl_code)) - {"GL_TEXTURE0", "GL_TEXTURE1", "GL_MSG_W"})
    constants = [n for n in constants if not n.startswith("GL_TEXFMT_")]
    render(templates / "shared_brightmap_gl.cpp.in", {
        "CONSTANTS": "enum { GL_TEXTURE0=0, GL_TEXTURE1=1, " + ", ".join(constants) + " };",
        "FUNCTIONS": gl_code,
    }, output / "shared_brightmap_gl.cpp")
    encore_parts = {
        "SELECT": function_section(hardware, "HWR_ProjectSprite",
            "\tINT32 skinnum = TC_DEFAULT;", "\n\t// set top/bottom coords"),
        "SOFTWARE_FULLBRIGHT": function_section(things, "R_DrawVisSprite",
            "\tdc.lightmap = colormaps;\n\n\tdc.fullbright = colormaps;", "\n\tdc.texturemid = vis->texturemid;"),
        "DRAW_COLUMN": cache.function("HWR_DrawColumnInCache"),
        "TEXTURE_PALETTE": cache.function("HWR_GetTexturePalette"),
        "CONVERT_LIGHT_TABLE": cache.function("HWR_ConvertLightTable"),
        "GET_PATCH": cache.function("HWR_GetPatch"),
    }
    for token, function in (("GET_MAPPED_PATCH", "HWR_GetMappedPatch"),
            ("LOAD_PATCH", "HWR_LoadPatchMipmap"), ("UPDATE_PATCH", "HWR_UpdatePatchMipmap")):
        encore_parts[token] = cache.function(function).replace(
            "grPatch = patch->hardware;", "grPatch = (GLPatch_t *)patch->hardware;").replace(
            "newMipmap = calloc(", "newMipmap = (GLMipmap_t *)calloc(").replace(
            "newMipmap->colormap = Z_Calloc(", "newMipmap->colormap = (GLColormap_t *)Z_Calloc(")
    render(templates / "encore_brightmap.cpp.in", encore_parts, output / "encore_brightmap.cpp")

    rotation = Source(engine / "src/r_patchrotation.c")
    patch_source = Source(engine / "src/r_patch.cpp")
    pictures = Source(engine / "src/r_picformats.c")
    # C allocation assignments gain explicit casts only for the C++ harness.
    def cpp_alloc(code):
        for lhs, typ in [("rotsprite", "rotsprite_t"), ("rotsprite->patches", "void *"),
                         ("rawdst", "UINT16"), ("rawconv", "UINT16"), ("pixels", "UINT16"),
                         ("imgbuf", "UINT8"), ("img", "UINT8")]:
            code = code.replace(lhs + " = Z_", lhs + " = (" + typ + " *)Z_")
        code = code.replace("? PICFLAGS_XFLIP : 0", "? PICFLAGS_XFLIP : PICFLAGS_NONE")
        code = code.replace(", 0, 0, 0);", ", 0, 0, PICFLAGS_NONE);")
        code = code.replace("patch = W_CachePatchNum", "patch = (patch_t *)W_CachePatchNum")
        code = code.replace("return rotsprite->patches[idx];", "return (patch_t *)rotsprite->patches[idx];")
        return code
    rotation_parts = "\n".join(cpp_alloc(rotation.function(name)) for name in [
        "RotatedPatch_Create", "RotatedPatch_CalculateDimensions", "RotatedPatch_Sample",
        "Patch_FreeInvalidatedBrightmaps", "Patch_InvalidateRotatedBrightmaps",
        "Patch_GetRotatedBrightmap", "RotatedPatch_DoRotation", "Patch_GetRotatedSprite",
        "RotatedPatch_Geometry", "Patch_GetRollGeometry"])
    picture_parts = "\n".join(cpp_alloc(pictures.function(name)) for name in [
        "Picture_FormatBPP", "Picture_IsInternalPatchFormat", "Picture_IsDoomPatchFormat",
        "Picture_IsPatchFormat", "Picture_IsFlatFormat", "Picture_GetPatchPixel",
        "Picture_PatchConvert", "Picture_FlatConvert", "Picture_Convert"])
    free_parts = "\n".join(patch_source.function(name) for name in [
        "Patch_Create", "Patch_FreeData", "Patch_Free", "Patch_FreeTagsCallback", "Patch_FreeTags"])
    sprite_parts = {
        "ROTATION": rotation_parts,
        "ROLL_SURFACE": hardware.function("HWR_RollSurface") + hardware.function("HWR_SpriteUV"),
        "ROLL_SHADER": Source(engine / "src/hardware/r_viberant/shaders.hlsl").function("RollSource").replace("return int2(", "return makeInt2("),
        "BASELINE": cpp_alloc(Source(Path(__file__).parent / "fixtures/sprite-rotation-baseline.c").section(
            "static void Baseline_CalculateDimensions", "\n// END")),
        "PICTURES": picture_parts,
        "PATCHES": free_parts,
        "TEXTURE_COORDS": function_section(hardware, "HWR_DrawSprite",
            "\tif (spr->flip)", "\n\n\t// This needs to be AFTER"),
        "CACHE_BRIGHT": things.function("R_CacheSpriteBrightMap"),
        # C permits these void-pointer assignments; add only the C++ harness cast.
        "LOAD_BRIGHT": cache.function("HWR_GetSpriteBrightmap").replace(
            "grPatch = patch->hardware;", "grPatch = (GLPatch_t *)patch->hardware;"),
        "LOAD_PATCH": cache.function("HWR_LoadPatchMipmap").replace(
            "grPatch = patch->hardware;", "grPatch = (GLPatch_t *)patch->hardware;"),
        "GET_PATCH": cache.function("HWR_GetPatch"),
        "SELECT": function_section(hardware, "HWR_ProjectSprite",
            "\n\trot = thing->frame&FF_FRAMEMASK;", "\n\tsprframe ="),
        "PROJECT": function_section(hardware, "HWR_ProjectSprite",
            "\tvis->rotated = false;", "\n\tvis->mobj = thing;"),
        "PROJECT_PRECIP": function_section(hardware, "HWR_ProjectPrecipitationSprite",
            "\tvis->gpatch =", "\n\tvis->flip ="),
        "CURRENT_TEXTURE": batching.function("HWR_SetCurrentTexture") + batch_helper,
        "BATCH_FIRST": function_section(batching, "HWR_RenderBatches",
            "\tif (currentPolyFlags & PF_NoTexture)", "\n\twhile (1)"),
        "BATCH_ROLL": function_section(batching, "HWR_RenderBatches",
            "#ifdef HAVE_VIBERANT\n\t\t\tif (HWR_VibeRant() && (((currentPolyFlags", "\n\t\t\tif (currentPolyFlags != nextPolyFlags)"),
        "BATCH_CHANGE": function_section(batching, "HWR_RenderBatches",
            "\t\t\tif (currentTexture != nextTexture || currentBrightmap != nextBrightmap)",
            "\n\t\t\tif (currentPolyFlags != nextPolyFlags)"),
        "BATCH_NEXT": function_section(batching, "HWR_RenderBatches",
            "\t\tif (changeTexture)", "\n\t\tif (changePolyFlags)"),
        "FIND_TEXTURE": driver12.function("FindTexture"),
        "BIND_TEXTURE": driver12.function("BindTexture"),
        "NO_TEXTURE": driver12.function("SetNoTexture"),
        "SET_TEXTURE": driver12.function("BindDownloadedTexture") + "\n" + driver12.function("SetTextureInternal"),
        "UPLOAD_BIND": function_section(driver12, "UpdateTextureInternal",
            "\tif (!(pTexInfo->flags & TF_BRIGHTMAP))", "\n\tTexture *texture = FindTexture(num);"),
    }
    for token, function, after in [
        ("DRAW", "HWR_DrawSprite", "\n\tif (spr->flip)"),
        ("SPLIT", "HWR_SplitSprite", "\n\tbaseWallVerts[0].x"),
        ("PRECIP", "HWR_DrawPrecipitationSprite", "\n\t// colormap test"),
    ]:
        sprite_parts[token] = function_section(hardware, function,
            "\tHWR_GetMappedPatch(gpatch, spr->colormap);", after)
    render(templates / "sprite_brightmap.cpp.in", sprite_parts, output / "sprite_brightmap.cpp")

    # Exercise the actual submission sites and endpoint recording, without
    # duplicating the large sprite light-list and seg traversal dependencies.
    split_sprite = hardware.function("HWR_SplitSprite")
    draw_sprite = hardware.function("HWR_DrawSprite")
    submission = "HWR_DrawSpringPolygon(&Surf, wallVerts, blend|PF_Modulated, shader, spr);"
    if split_sprite.count(submission) != 2 or "HWR_ProcessPolygon(" in split_sprite:
        raise ValueError("spring light-split sprite submissions bypass silhouette clipping")
    if draw_sprite.count(submission) != 1:
        raise ValueError("spring ordinary sprite submission bypasses silhouette clipping")
    split_calls = []
    split_code = split_sprite.split("\n", 1)[1]
    split_offset = hardware.text.index(split_code.rstrip("\n"))
    for match in re.finditer(re.escape(submission), split_code):
        split_calls.append(hardware.annotated(split_offset + match.start(),
            split_offset + match.end()))
    routes = ("static void SpringSplitPiece(FSurfaceInfo *surface, FOutVector *wallVerts, "
        "FBITFIELD blend, INT32 shader, gl_vissprite_t *spr, bool final) {\n"
        "FSurfaceInfo &Surf = *surface;\nif (final) {\n" + split_calls[1] +
        "} else {\n" + split_calls[0] + "}\n}\n")
    solid = function_section(hardware, "HWR_ProcessSeg",
        "\t\t// [ZE]: Software clips springs to this solid seg's silhouette.",
        "\t\t// Single sided line")
    routes += ("static void SpringRecordSolid(FOutVector *wallVerts, fixed_t worldtop, "
        "fixed_t worldtopslope, fixed_t worldbottom, fixed_t worldbottomslope) {\n"
        + solid + "}\n")
    for kind, top, bottom in (("Upper", "worldtop", "worldhigh"), ("Lower", "worldlow", "worldbottom")):
        section = function_section(hardware, "HWR_ProcessSeg",
            f"\t\t\t// set top/bottom coords\n\t\t\twallVerts[3].y = FIXED_TO_FLOAT({top});",
            "\n\t\t\t{\n\t\t\t\tFBITFIELD polyflags = PF_Masked;")
        if "HWR_AddSpringWall(wallVerts, NULL);" not in section:
            raise ValueError(f"two-sided {kind.lower()} walls must be recorded for spring clipping")
        routes += (f"static void SpringRecord{kind}(FOutVector *wallVerts, fixed_t {top}, "
            f"fixed_t {top}slope, fixed_t {bottom}, fixed_t {bottom}slope) {{\n" + section + "}\n")
    process_seg = hardware.function("HWR_ProcessSeg").split("\n", 1)[1].rstrip("\n")
    seg_offset = hardware.text.index(process_seg)
    record_start = "\t\t\t\t\t// [ZE]: Opaque solid FOF silhouette before lighting splits."
    record_end = "\t\t\t\t\t\tHWR_AddSpringWall(wallVerts, rover);\n"
    starts = [m.start() for m in re.finditer(re.escape(record_start), process_seg)]
    if len(starts) != 2:
        raise ValueError("expected two opaque FOF side recording routes")
    for index, start in enumerate(starts):
        end = process_seg.index(record_end, start) + len(record_end)
        recording = process_seg[start:end]
        if "if (!(grTex->mipmap.flags & TF_TRANSPARENT))" not in recording:
            raise ValueError("masked FOF holes must retain ordinary depth-tested behavior")
        # The recording must come before this side is split by sector lights.
        if "HWR_SplitWall(" in recording or "HWR_SplitWall(" not in process_seg[end:end + 200]:
            raise ValueError("opaque FOF recording must precede light slicing")
        routes += (f"static void SpringRecordFOF{index}(FOutVector *wallVerts, ffloor_t *rover) {{\n"
            + hardware.annotated(seg_offset + start, seg_offset + end) + "}\n")
    # The 3D floor plane recorder reads the subsector's polygon and each corner's height. It must run for the
    # opaque planes HWR_Subsector actually draws: the bottom and the top one.
    subsector = hardware.function("HWR_Subsector")
    for isceiling, pic in (("false", "bottompic"), ("true", "toppic")):
        if (f"HWR_AddSpringFOFPlane(&extrasubsectors[num], rover, {isceiling}); // [ZE]\n"
            f"\t\t\t\t\tHWR_GetLevelFlat(&levelflats[*rover->{pic}]") not in subsector:
            raise ValueError(f"the opaque 3D floor {pic} plane must be recorded for spring clipping")
    routes += hardware.function("HWR_AddSpringFOFPlane")
    quick_reject = hardware.function("HWR_SpringQuickReject")
    plain_depth = ": nearest > view->depth"
    if quick_reject.count(plain_depth) != 1:
        raise ValueError("expected one plain spring side depth test")
    strict_depth = (
        quick_reject.replace(plain_depth, ": nearest >= view->depth")
        + hardware.function("HWR_SpringWallApplies")
        + hardware.function("HWR_SpringSubtract")
    )
    for name in ("SpringQuickReject", "SpringWallApplies", "SpringSubtract"):
        strict_depth = strict_depth.replace(f"HWR_{name}", f"StrictDepth{name}")
    render(templates / "spring_clip.cpp.in", {
        "HELPERS": hardware.section("// [ZE]: Software hides a spring that sits inside a wall.",
            "// End [ZE] spring silhouette geometry."),
        "REFERENCE": (templates.parent / "fixtures" / "spring-clip-reference.inc").read_text(encoding="utf-8"),
        "STRICT_DEPTH": strict_depth,
        "DRAW_SPRITE": hardware.function("HWR_SpriteUV") + draw_sprite,
        "ROUTES": routes,
        "MODEL": hardware.section("// How a spring's 3D model is to be drawn, from what Software leaves of the spring's sprite.",
            "static void HWR_DrawVisSprite(gl_vissprite_t *spr)"),
    }, output / "spring_clip.cpp")

    render(templates / "splat_clear.cpp.in", {
        "SPLAT": hardware.section("// [ZE]: How far the floor under a floor sprite rises over the sprite",
            "static void HWR_DrawSprite(gl_vissprite_t *spr)"),
        "CALL": function_section(hardware, "HWR_DrawSprite", "\tHWR_SpriteVertices(spr, wallVerts);",
            "\n\n\t// cache the patch in the graphics card memory"),
    }, output / "splat_clear.cpp")

    render(templates / "distortion.cpp.in", {
        "VERTEX": Source(engine / "src/hardware/r_viberant/viberant_screens.inc").function("PostImageVertex"),
        "POSTPROCESS": hardware.function("HWR_DoPostProcessor"),
    }, output / "distortion.cpp")

    water = Source(engine / "src/hardware/r_viberant/shaders.hlsl")
    def native_water(name):
        code = water.function(name).replace(" : SV_Target", "")
        code = code.replace("int3(", "makeint3(").replace("float4(", "make4(")
        return re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", code)
    render(templates / "water.cpp.in", {
        "OFFSET": native_water("WaterBackgroundOffset"),
        "BLEND": native_water("BlendWaterScene"),
        "WATER_SHADER": native_water("PSWaterRefraction"),
        "SOFTWARE_OFFSET": Source(engine / "src/r_plane.cpp").function("R_CalculateRippleOffset"),
        "FIXED_DIV": Source(engine / "src/m_fixed.c").function("FixedDiv2"),
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
    }, output / "water.cpp")


    fixture = json.loads((templates.parent / "fixtures/water-palace-water.json").read_text())
    fixed = lambda value: str(int(value * 65536))
    fixture_code = ["const std::vector<Water> mapWater={"]
    for w in fixture["waters"]:
        fixture_code.append(f"    {{{w['line']},{fixed(w['bottom'])},{fixed(w['top'])}}},")
    fixture_code.append("};\nconst std::vector<SectorCase> mapSectors={")
    for case in fixture["sectors"]:
        lines = ",".join(map(str, case["water_lines"]))
        fixture_code.append(f"    {{{case['sector']},{fixed(case['floor'])},{fixed(case['ceiling'])},{{{lines}}}}},")
    fixture_code.append("};")
    software = Source(engine / "src/r_plane.cpp")
    render(templates / "water_visibility.cpp.in", {
        "HIDDEN": hardware.function("HWR_FOFPlaneIsHidden"),
        "SOFTWARE": software.section("\t\t\t// Don't draw planes that shouldn't be drawn.",
            "\n\t\t\tif (pl->ffloor->fofflags & FOF_TRANSLUCENT)").replace("return;", "return true;"),
        "TOP": hardware.section("\t\t\tif (!HWR_FOFPlaneIsHidden(rover, *rover->topheight)", "\n\t\t\t{"),
        "BOTTOM": hardware.section("\t\t\tif (!HWR_FOFPlaneIsHidden(rover, *rover->bottomheight)", "\n\t\t\t{"),
        "FIXTURE": "\n".join(fixture_code),
    }, output / "water_visibility.cpp")


    render(templates / "sky.cpp.in", {
        "SETUP": Source(engine / "src/r_sky.c").function("R_SetupSkyDraw"),
        "OFFSETS": hardware.function("HWR_ApplySkyTextureOffsets"),
        "VERTEX": hardware.function("HWR_SkyDomeVertex"),
    }, output / "sky.cpp")

    bsp = Source(engine / "src/r_bsp.cpp")
    fixture = json.loads((templates.parent / "fixtures/water-palace-horizons.json").read_text())
    names = {"F_SKY1": 1, "~015": 2, "WCZFLA01": 3}
    fixture_code = []
    for case in fixture["horizons"]:
        fixture_code.append(f"    {{ // Water Palace line {case['line']}, sectors {case['front_sector']}/{case['back_sector']}")
        fixture_code.append("        sector_t a=BlankSector(),b=BlankSector();")
        for variable, fields in (("a", case["front"]), ("b", case["back"])):
            for key, member in (("heightfloor", "floorheight"), ("heightceiling", "ceilingheight")):
                fixture_code.append(f"        {variable}.{member}={int(float(fields[key])*65536)};")
            for key, member in (("texturefloor", "floorpic"), ("textureceiling", "ceilingpic")):
                fixture_code.append(f"        {variable}.{member}={names[fields[key]]};")
            fixture_code.append(f"        {variable}.lightlevel={fields['lightlevel']};")
            if "damagetype" in fields:
                if fields["damagetype"] != "DeathPit":
                    raise ValueError("Unexpected fixture damage type")
                fixture_code.append(f"        {variable}.damagetype=SD_DEATHPIT;")
            # The actual horizon sectors have default offsets, slope pointers,
            # lighting overrides and no tags. Fail if the fixture changes.
            allowed = {"heightfloor", "heightceiling", "texturefloor", "textureceiling", "lightlevel", "damagetype"}
            defaults = {"yscalefloor": "1.0", "yscaleceiling": "1.0", "xscalefloor": "1.0", "xscaleceiling": "1.0",
                        "xpanningfloor": "0.0", "ypanningfloor": "0.0", "xpanningceiling": "0.0", "ypanningceiling": "0.0",
                        "rotationfloor": "0.0", "rotationceiling": "0.0"}
            if any(k not in allowed and fields[k] != defaults.get(k) for k in fields):
                raise ValueError("Unexpected fixture surface properties")
        if case["side"].get("texturemiddle", "-") != "-":
            raise ValueError("Unexpected fixture middle texture")
        fixture_code.extend(["        seg.backsector=&b;",
            "        bool extend=RenderHorizonCondition(&seg,&a);",
            "        assert(extend==SoftwareDrawsLine(&seg,&a));",
            "        ++cases;if(!extend)++skipped;", "    }"])
    render(templates / "front_bbox.cpp.in", {
        "BSP_WALK": hardware.function("HWR_RenderBSPNode"),
    }, output / "front_bbox.cpp")
    for name in ("HWR_RenderPlane", "HWR_ProjectWall", "HWR_DrawFlatBleed",
                 "HWR_RenderPolyObjectPlane", "HWR_RenderWall"):
        assert hardware.function(name).count("HWR_AdjustLevelLight(") == 1
    assert hardware.text.count("HWR_AdjustLevelLight(") == 6
    render(templates / "level_darkness.cpp.in", {
        "SOFTWARE": Source(engine / "src/r_debug.cpp").function("R_AdjustLightLevel"),
        "ADJUST": hardware.function("HWR_AdjustLevelLight"),
    }, output / "level_darkness.cpp")
    assert hardware.function("HWR_ProcessSeg").count("HWR_MidtextureSkyCuts(") == 2
    process = hardware.function("HWR_ProcessSeg")
    assert process.count("HWR_MidtextureFog(") == 1
    assert "if (HWR_MidtextureFog(gl_curline->polyseg != NULL, tripwire,\n\t\t\t\tgl_linedef, gl_frontsector, wallVerts, &Surf, lightnum, colormap))" in process
    fog_call = process.index("if (HWR_MidtextureFog(")
    assert process.rindex("wallVerts[1].y = FIXED_TO_FLOAT(l);", 0, fog_call) < fog_call
    assert "; // Fog (including invisible fog) replaces the textured draw.\n\t\t\telse if (tripwire == false && gl_frontsector->numlights)" in process
    render(templates / "midtexture_fog.cpp.in", {
        "FOG": hardware.function("HWR_MidtextureFog"),
        "TRANS": Source(engine / "src/r_segs.cpp").function("R_GetLinedefTransTable"),
    }, output / "midtexture_fog.cpp")
    assert process.count("HWR_MidtextureBeforeSky(") == 1
    # Every single-bit flag of EPolyFlags, so a flag added later is checked against PF_BeforeSky too.
    poly_flags = re.findall(r"^\t(PF_\w+)\s*=\s*0x[0-9A-Fa-f]+,", Source(engine / "src/hardware/hw_defs.h").text, re.MULTILINE)
    assert len(poly_flags) >= 24 and "PF_BeforeSky" in poly_flags
    render(templates / "midtexture_sky.cpp.in", {
        "CUTS": hardware.function("HWR_MidtextureSkyCuts"),
        "BEFORE_SKY": hardware.function("HWR_MidtextureBeforeSky"),
        "CALLER": function_section(hardware, "HWR_ProcessSeg",
            "\t\t\tblendmode |= HWR_MidtextureBeforeSky(", "\n"),
        "FLAGS": ", ".join(poly_flags),
    }, output / "midtexture_sky.cpp")
    render(templates / "sky_claim.cpp.in", {
        "SKY_GATE": function_section(hardware, "HWR_Subsector",
            "\tif (HWR_VibeRant() && sub->validcount != validcount)\n\t{\n\t\tif (gl_frontsector->floorpic == skyflatnum",
            "\n#endif"),
    }, output / "sky_claim.cpp")
    render(templates / "horizon_cache.cpp.in", {
        "CACHE": hardware.section("static UINT8 *hwr_horizonflags;", "#endif\n\n#ifdef DOPLANES"),
        "PLANE_GATE": function_section(hardware, "HWR_RenderPlane", "\tif (subsector\n#ifdef HAVE_VIBERANT", "\n\t{\n\t\t// Horizon lines"),
        "SKY_GATE": function_section(hardware, "HWR_RenderSkyClaim", "\tif (HWR_SubsectorHasHorizon(subsector))", "\n\t{\n\t\tconst float renderdist"),
    }, output / "horizon_cache.cpp")
    decision = bsp.section("\tbacksector = R_FakeFlat(backsector, &tempsec, NULL, NULL, true);", "\nclippass:")
    render(templates / "horizon.cpp.in", {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "SLOPE_HEIGHT": Source(engine / "src/p_slopes.c").function("P_GetSlopeZAt"),
        "HEIGHT": Source(engine / "src/p_slopes.c").function("P_GetZAt"),
        "TAGS": Source(engine / "src/taglist.c").section("boolean Tag_Compare (", "/// Search for an element"),
        "FAKE_FLAT": bsp.function("R_FakeFlat"),
        "DEBUG_LINE": bsp.function("R_IsDebugLine"),
        "EMPTY_LINE": bsp.function("R_IsEmptyLine"),
        "EXTEND": hardware.function("HWR_ShouldExtendHorizon"),
        "DRAW_CONDITION": hardware.section("\t\t\tif (HWR_ShouldExtendHorizon", "\n\t\t\t{"),
        "SOFTWARE_DECISION": decision.replace("return;", "return false;"),
        "FIXTURE": "\n".join(fixture_code),
    }, output / "horizon.cpp")

    slopes = Source(engine / "src/p_slopes.c")
    render(templates / "flat_bleed.cpp.in", {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "SLOPE_HEIGHT": slopes.function("P_GetSlopeZAt"),
        "SECTOR_FLOOR": slopes.function("P_GetSectorFloorZAt"),
        "SECTOR_CEILING": slopes.function("P_GetSectorCeilingZAt"),
        "FLAT_BLEEDS": hardware.function("HWR_FlatBleeds"),
        "FLAT_UV": hardware.function("HWR_FlatBleedUV"),
        "FLAT_DRAW": hardware.function("HWR_AdjustLevelLight") + hardware.function("HWR_DrawFlatBleed"),
        "FLAT_DIMENSIONS": Source(engine / "src/r_textures.cpp").function("R_FlatDimensionsFromLumpSize"),
    }, output / "flat_bleed.cpp")

    render(templates / "slope_light.cpp.in", {
        "USE_SHADER": hardware.function("HWR_UseShader"),
        "STEPS": hardware.function("HWR_SoftwareLightSteps"),
        # Software's range rule for an offset, with the sector's lighting model stubbed
        "REAL_RULE": Source(engine / "src/p_setup.cpp").function("P_ApplyLightOffset"),
        "CALC": hardware.function("HWR_CalcSlopeLight"),
        # The statement itself, so that the test breaks if the expression in it changes
        "PLANE_CALL": hardware.section("lightlevel = HWR_CalcSlopeLight(", "\n"),
    }, output / "slope_light.cpp")

    render(templates / "wall_light_steps.cpp.in", {
        "USE_SHADER": hardware.function("HWR_UseShader"),
        "STEPS": hardware.function("HWR_SoftwareLightSteps"),
        "REAL_RULE": Source(engine / "src/p_setup.cpp").function("P_ApplyLightOffset"),
        "CALC": hardware.function("HWR_StepWallLight") + hardware.function("HWR_CalcWallLight"),
        "SKIPS_OFFSET": hardware.function("HWR_SoftwareSkipsWallOffset"),
        "SLICE_LIGHT": hardware.function("HWR_SliceWallLight"),
        "FOG_LIGHT": hardware.function("HWR_FogColormapWallLight"),
    }, output / "wall_light_steps.cpp")

    render(templates / "void_wall.cpp.in", {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "SLOPE_HEIGHT": slopes.function("P_GetSlopeZAt"),
        "SECTOR_FLOOR": slopes.function("P_GetSectorFloorZAt"),
        "SECTOR_CEILING": slopes.function("P_GetSectorCeilingZAt"),
        "ROLL_ZOOM": hardware.function("HWR_SoftwareRollZoom"),
        "CLIP_AIM": Source(engine / "src/g_game.c").function("G_SoftwareClipAimingPitch"),
        "WALL_FILLS": hardware.function("HWR_WallFillsView"),
        "ADD_LINE_RULE": function_section(hardware, "HWR_AddLine",
            "\t\telse if (HWR_VibeRant() && portaloccludes && (voidwalls", "\n#endif\n\t\t// Reject empty lines"),
    }, output / "void_wall.cpp")

    render(templates / "batch_sort.cpp.in", {
        "BATCHING": batch_code,
    }, output / "batch_sort.cpp")

    postimg_mobj = Source(engine / "src/p_mobj.c")
    render(templates / "postimg_selection.cpp.in", {
        "CAMERA_HEAT": postimg_mobj.function("P_CameraCheckHeat"),
        "CAMERA_WATER": postimg_mobj.function("P_CameraCheckWater"),
        "CHASE": postimg_mobj.function("P_CalcChasePostImg"),
        "FIRST_PERSON": Source(engine / "src/p_user.c").function("P_CalcPostImg"),
    }, output / "postimg_selection.cpp")

    postimg = Source(engine / "src/hardware/r_viberant/shaders.hlsl").function("PostImageUV")
    postimg = re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", postimg)
    reference = Source(Path(__file__).parent / "fixtures/rhi_glsl_fragment_postimg.glsl").section(
        "\tvec2 texcoord0 = v_texcoord0;", "\n#ifdef ENABLE_S_SAMPLER1")
    reference = reference.replace("vec2(", "make2(").replace("vec2 ", "float2 ")
    reference = re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", reference)
    render(templates / "postimg.cpp.in", {"SHADER": postimg, "REFERENCE": reference}, output / "postimg.cpp")

    fps = Source(engine / "src/r_fps.cpp")
    render(templates / "view_interp.cpp.in", {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "STATE": fps.section("static viewvars_t pview_old[", "static std::vector<levelinterpolator_t>"),
        "LERP_FIXED": fps.function("R_LerpFixed"),
        "LERP_ANGLE": fps.function("R_LerpAngle"),
        "INTERPOLATE": fps.function("R_InterpolateView"),
        "UPDATE": fps.function("R_UpdateViewInterpolation"),
        "RESET": fps.function("R_ResetViewInterpolation"),
        "CONTEXT": fps.function("R_SetViewContext"),
    }, output / "view_interp.cpp")

    render(templates / "point_subsector.cpp.in", {
        "POINT_ON_SIDE": renderer.function("R_PointOnSideOf") + renderer.function("R_PointOnSide"),
        "BUILD": Source(engine / "src/p_setup.cpp").function("P_BuildNodeWalk"),
        "CACHE": renderer.section("#ifdef _UWP\n// [UWP]: The subsectors of points looked up recently",
            "//\n// R_PointInSubsectorOrNull"),
    }, output / "point_subsector.cpp")

    setup = Source(engine / "src/p_setup.cpp")
    build = setup.function("P_BuildNodeWalk")
    copy_children = "nodewalk[i].children[0] = nodes[i].children[0];"
    copy_dx = "nodewalk[i].dx = nodes[i].dx;"
    if build.count(copy_children) != 1 or build.count(copy_dx) != 1:
        raise ValueError("Could not inject the node walk negative controls")
    render(templates / "node_walk.cpp.in", {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "POINT_ON_SIDE": renderer.function("R_PointOnSideOf"),
        "WALK": renderer.section("#ifdef _UWP\n// [UWP]: The subsectors of points looked up recently",
            "//\n// R_PointInSubsectorOrNull"),
        "BUILD": build,
        "BUILD_SWAPPED_CHILDREN": build.replace("P_BuildNodeWalk", "P_BuildNodeWalkSwappedChildren")
            .replace(copy_children, "nodewalk[i].children[0] = nodes[i].children[1];"),
        "BUILD_DY_AS_DX": build.replace("P_BuildNodeWalk", "P_BuildNodeWalkDyAsDx")
            .replace(copy_dx, "nodewalk[i].dx = nodes[i].dy;"),
        "REFERENCE": (templates.parent / "fixtures/node-walk-baseline.inc").read_text(encoding="utf-8"),
    }, output / "node_walk.cpp")

    side = setup.function("P_BSPCellSide")
    cells = setup.function("P_BuildBSPCellStart")
    renderer_text = {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "POINT_ON_SIDE": renderer.function("R_PointOnSideOf") + renderer.function("R_PointOnSide"),
        "WALK": renderer.section("#ifdef _UWP\n// [UWP]: The subsectors of points looked up recently",
            "//\n// R_PointInSubsectorOrNull"),
        "BUILD_WALK": build,
        "BUILD": side + cells,
        "REFERENCE": (templates.parent / "fixtures/node-walk-baseline.inc").read_text(encoding="utf-8"),
    }

    def wrong_cells(suffix, in_side=(), in_cells=()):
        # A builder with one deliberate mistake: its own copy of both functions, with the given (old, new) edits.
        mine, theirs = side, cells
        for old, new in in_side:
            if mine.count(old) != 1:
                raise ValueError(f"Could not inject the BSP cell start negative control {suffix}: {old!r}")
            mine = mine.replace(old, new)
        for old, new in in_cells:
            if theirs.count(old) != 1:
                raise ValueError(f"Could not inject the BSP cell start negative control {suffix}: {old!r}")
            theirs = theirs.replace(old, new)
        return (mine + theirs).replace("P_BSPCellSide", "P_BSPCellSide" + suffix).replace(
            "P_BuildBSPCellStart", "P_BuildBSPCellStart" + suffix)

    corner_side = "INT32 here = R_PointOnSide((fixed_t)xs[i][c & 1], (fixed_t)ys[j][c >> 1], node);"
    overflow_check = "if (xlo - node->x < INT32_MIN || xhi - node->x > INT32_MAX || ylo - node->y < INT32_MIN || yhi - node->y > INT32_MAX)"
    render(templates / "bsp_cell_start.cpp.in", {
        **renderer_text,
        "BUILD_TWO_CORNERS": wrong_cells("TwoCorners", in_side=[("c < 4", "c < 2")]),
        "BUILD_NO_MASK": wrong_cells("NoMask", in_side=[
            (corner_side, "INT32 here = FixedMul((fixed_t)ys[j][c >> 1] - node->y, node->dx >> FRACBITS) >= "
                          "FixedMul(node->dy >> FRACBITS, (fixed_t)xs[i][c & 1] - node->x);")]),
        "BUILD_SHORT_EDGE": wrong_cells("ShortEdge", in_cells=[("INT64 xhi = xlo + cellsize - 1;", "INT64 xhi = xlo + cellsize - 2;")]),
        "BUILD_NO_OVERFLOW_CHECK": wrong_cells("NoOverflowCheck", in_side=[(overflow_check, "if (false)")]),
    }, output / "bsp_cell_start.cpp")
    generate_bsp(templates.parent / "fixtures", output)

    shader = Source(engine / "src/hardware/r_viberant/shaders.hlsl").function("PSWipeFull")
    shader = shader.replace(" : SV_Target", "")
    for size in (2, 3, 4):
        shader = shader.replace(f"float{size}(", f"make{size}(")
    # Clang vector/scalar arithmetic requires a matching float scalar type.
    shader = re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", shader)
    render(templates / "wipe.cpp.in", {"WIPE_SHADER": shader}, output / "wipe.cpp")

    bsp = Source(engine / "src/r_bsp.cpp")
    render(templates / "ripples.cpp.in", {
        "RIPPLE_PLANE": bsp.function("R_IsRipplePlane"),
        "USE_SHADER": hardware.function("HWR_UseShader"),
        "POLYOBJECT_PLANE": hardware.function("HWR_AdjustLevelLight") + hardware.function("HWR_RenderPolyObjectPlane"),
        "RIPPLE_BLEND": hardware.function("HWR_RippleBlend"),
    }, output / "ripples.cpp")
    hardware = Source(engine / "src/hardware/hw_main.c")
    render(templates / "transparency.cpp.in", {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "SLOPE_Z": Source(engine / "src/p_slopes.c").function("P_GetSlopeZAt"),
        "PAPER_SPRITE": Source(engine / "src/r_things.cpp").function("R_ThingIsPaperSprite"),
        "FLOOR_SPRITE": Source(engine / "src/r_things.cpp").function("R_ThingIsFloorSprite"),
        "MOBJ_FLIP": Source(engine / "src/p_mobj.c").function("P_MobjFlip"),
        "TRANSFORM": hardware.function("transform"),
        "BILLBOARD": hardware.function("HWR_RotateSpritePolyToAim"),
        "SHADOW_SHEAR": hardware.function("HWR_ShadowDrawShear"),
        "SPRITE_VERTICES": hardware.function("HWR_SpriteVertices"),
        "DRAW_NODES": hardware.section("// A drawnode is something", "//\n// HWR_CreateDrawNodes\n"),
        "ADD_WALL": hardware.function("HWR_AddTransparentWall"),
        "CREATE_NODES": hardware.function("HWR_CreateDrawNodes"),
        "BEGIN_SPRITES": hardware.function("HWR_BeginSpriteDraw"),
        "END_SPRITES": hardware.function("HWR_EndSpriteDraw"),
        "SPRITE_SHADOW": hardware.function("HWR_DrawSpriteShadow"),
        "DRAW_SPRITE": hardware.function("HWR_DrawVisSprite"),
        "DRAW_SPRITES": hardware.function("HWR_DrawSprites"),
    }, output / "transparency.cpp")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.engine.resolve(), args.output.resolve())
