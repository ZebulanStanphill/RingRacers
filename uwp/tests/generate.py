#!/usr/bin/env python3
"""Generate small native harnesses from the actual patched engine sources."""

import argparse
import json
import re
from pathlib import Path

from source import Source


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


def generate(engine, output):
    output.mkdir(parents=True, exist_ok=True)
    templates = Path(__file__).parent / "templates"
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

    cache = Source(engine / "src/hardware/hw_cache.c")
    render(templates / "pictures.cpp.in", {
        "PICTURE_TABLES": cache.section("// Source pixels and GPU pixels", "static void HWR_DrawPicInCache"),
        "CONVERT": cache.function("HWR_DrawPicInCache"),
        "GET_PIC": cache.function("HWR_GetPic"),
    }, output / "pictures.cpp")

    driver = Source(engine / "src/hardware/r_d3d11/r_d3d11.cpp")
    hardware = Source(engine / "src/hardware/hw_main.c")
    render(templates / "distortion.cpp.in", {
        "VERTEX": driver.function("PostImageVertex"),
        "POSTPROCESS": hardware.function("HWR_DoPostProcessor"),
    }, output / "distortion.cpp")

    water = Source(engine / "src/hardware/r_d3d11/shaders.hlsl")
    driver = Source(engine / "src/hardware/r_d3d11/r_d3d11.cpp")
    def native_water(name):
        code = water.function(name).replace(" : SV_Target", "")
        code = code.replace("int3(", "makeint3(").replace("float4(", "make4(")
        return re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", code)
    render(templates / "water.cpp.in", {
        "OFFSET": native_water("WaterBackgroundOffset"),
        "BLEND": native_water("BlendWaterScene"),
        "WATER_SHADER": native_water("PSWaterRefraction"),
        "BOUNDS": driver.function("WaterCaptureBounds"),
        "SOFTWARE_OFFSET": Source(engine / "src/r_plane.cpp").function("R_CalculateRippleOffset"),
        "FIXED_DIV": Source(engine / "src/m_fixed.c").function("FixedDiv2"),
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
    }, output / "water.cpp")

    render(templates / "water_capture.cpp.in", {
        "MULTIPLY": driver.function("Multiply"),
        "CAPTURE_GLOBALS": driver.section("ComPtr<ID3D11Texture2D> g_waterScene;", "// Conservative projected bounds."),
        "BOUNDS": driver.function("WaterCaptureBounds"),
        "CAPTURE": driver.function("CaptureWaterScene"),
        "CONTINUES": hardware.function("HWR_WaterPlaneContinues"),
        "DISPATCH": hardware.section("\t\tconst planeinfo_t *plane = sortnode[sortindex[i]].plane;", "\n#endif"),
        "OFFSET": native_water("WaterBackgroundOffset"),
        "BLEND": native_water("BlendWaterScene"),
        "WATER_SHADER": native_water("PSWaterRefraction"),
    }, output / "water_capture.cpp")

    render(templates / "blend.cpp.in", {
        "ENUMS": driver.section("enum BlendFactor : UINT8", "enum AlphaFunc"),
        "TO_BLEND": driver.function("ToD3DBlend"),
        "BLEND_STATE": driver.function("GetBlendState"),
    }, output / "blend.cpp")

    render(templates / "sky.cpp.in", {
        "SETUP": Source(engine / "src/r_sky.c").function("R_SetupSkyDraw"),
        "OFFSETS": hardware.function("HWR_ApplySkyTextureOffsets"),
        "VERTEX": hardware.function("HWR_SkyDomeVertex"),
    }, output / "sky.cpp")

    postimg = Source(engine / "src/hardware/r_d3d11/shaders.hlsl").function("PostImageUV")
    postimg = re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", postimg)
    reference = Source(Path(__file__).parent / "fixtures/rhi_glsl_fragment_postimg.glsl").section(
        "\tvec2 texcoord0 = v_texcoord0;", "\n#ifdef ENABLE_S_SAMPLER1")
    reference = reference.replace("vec2(", "make2(").replace("vec2 ", "float2 ")
    reference = re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", reference)
    render(templates / "postimg.cpp.in", {"SHADER": postimg, "REFERENCE": reference}, output / "postimg.cpp")

    shader = Source(engine / "src/hardware/r_d3d11/shaders.hlsl").function("PSWipeFull")
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
        "POLYOBJECT_PLANE": hardware.function("HWR_RenderPolyObjectPlane"),
        "RIPPLE_BLEND": hardware.function("HWR_RippleBlend"),
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
    }, output / "ripples.cpp")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.engine.resolve(), args.output.resolve())
