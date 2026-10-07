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

    shader = Source(engine / "src/hardware/r_d3d11/shaders.hlsl").function("PSWipeFull")
    shader = shader.replace(" : SV_Target", "")
    for size in (2, 3, 4):
        shader = shader.replace(f"float{size}(", f"make{size}(")
    # Clang vector/scalar arithmetic requires a matching float scalar type.
    shader = re.sub(r"(?<![\w.])(\d+\.\d+)(?![\w.])", r"\1f", shader)
    render(templates / "wipe.cpp.in", {"WIPE_SHADER": shader}, output / "wipe.cpp")

    hardware = Source(engine / "src/hardware/hw_main.c")
    render(templates / "transparency.cpp.in", {
        "FIXED_MUL": Source(engine / "src/m_fixed.c").function("FixedMul"),
        "SLOPE_Z": Source(engine / "src/p_slopes.c").function("P_GetSlopeZAt"),
        "PAPER_SPRITE": Source(engine / "src/r_things.cpp").function("R_ThingIsPaperSprite"),
        "FLOOR_SPRITE": Source(engine / "src/r_things.cpp").function("R_ThingIsFloorSprite"),
        "MOBJ_FLIP": Source(engine / "src/p_mobj.c").function("P_MobjFlip"),
        "TRANSFORM": hardware.function("transform"),
        "BILLBOARD": hardware.function("HWR_RotateSpritePolyToAim"),
        "SPRITE_VERTICES": hardware.function("HWR_SpriteVertices"),
        "DRAW_NODES": hardware.section("// A drawnode is something", "//\n// HWR_CreateDrawNodes\n"),
        "ADD_WALL": hardware.function("HWR_AddTransparentWall"),
        "CREATE_NODES": hardware.function("HWR_CreateDrawNodes"),
        "BEGIN_SPRITES": hardware.function("HWR_BeginSpriteDraw"),
        "END_SPRITES": hardware.function("HWR_EndSpriteDraw"),
        "SPRITE_SHADOW": hardware.function("HWR_DrawSpriteShadow"),
        "DRAW_SPRITE": hardware.function("HWR_DrawVisSprite"),
        "DRAW_SPRITES": hardware.function("HWR_DrawSprites"),
        "LINK_ADD": hardware.function("HWR_LinkDrawHackAdd"),
    }, output / "transparency.cpp")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.engine.resolve(), args.output.resolve())
