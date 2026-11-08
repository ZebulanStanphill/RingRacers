"""Extract the title replay's production dependencies; no engine bodies are stored here."""

from source import Source


def generate_title(engine, output, templates, render, function_section):
    engine = engine / "src"
    hw = Source(engine / "hardware/hw_main.c")
    sl = Source(engine / "p_slopes.c")
    bsp = Source(engine / "r_bsp.cpp")
    prep = function_section(
        hw,
        "HWR_Subsector",
        "\t\tboolean anyMoved = gl_frontsector->moved;",
        "\n\t\tlight = R_GetPlaneLight",
    )
    parts = {}
    code = Source(engine / "taglist.c").section(
        "boolean Tag_Compare", "/// Search for an element"
    ) + Source(engine / "r_main.cpp").function("R_PointOnSide")
    for n in [
        "P_GetSlopeZAt",
        "P_GetZAt",
        "P_GetSectorFloorZAt",
        "P_GetSectorCeilingZAt",
        "P_GetFFloorTopZAt",
        "P_GetFFloorBottomZAt",
        "P_GetLightZAt",
    ]:
        code += sl.function(n)
    # Flat continuation is covered by flat_bleed; the title replay records wall submissions only.
    code += "\nstatic UINT8 HWR_FlatBleeds(const sector_t *, const sector_t *, fixed_t, fixed_t, fixed_t, fixed_t, boolean, boolean, boolean, INT32, fixed_t, fixed_t, fixed_t) { return 0; }\n"
    code += "\nstatic void HWR_DrawFlatBleed(boolean, fixed_t, fixed_t, fixed_t, fixed_t, const FOutVector *) {}\n"
    for n in [
        "HWR_FogBlockAlpha",
        "HWR_DrawWallLightSlice",
        "HWR_SplitWall",
        "HWR_ProcessSeg",
    ]:
        code += hw.function(n)
    code += (
        "\n#define R_Prep3DFloors R_Prep3DFloors_Impl\n"
        + bsp.function("R_Prep3DFloors")
        + "\n#undef R_Prep3DFloors\n"
    )
    parts["WALL_LIGHTING"] = code
    code = ""

    code += (
        Source(engine / "tables.c").function("SlopeDivEx")
        + '\n#include "t_tan2a.c"\n'
        + Source(engine / "r_main.cpp").function("R_PointToAngle64")
    )

    parts["POINT_ANGLE"] = code
    code = ""
    code += Source(engine / "tables.c").function("SlopeDiv")
    parts["SLOPE_TABLE"] = code
    code = ""
    for n in [
        "FixedDiv2",
        "FixedHypot",
        "FV3_Copy",
        "FV3_SubEx",
        "FV3_DivideEx",
        "FV3_Divide",
        "FV3_Cross",
    ]:
        code += Source(engine / "m_fixed.c").function(n)
    for n in ["R_PointToAngle2", "R_PointToDist2"]:
        code += Source(engine / "r_main.cpp").function(n)
    code += sl.function("P_UpdateSlopeLightOffset")
    parts["SLOPE_MATH"] = code
    code = ""
    code += sl.section(
        "static inline pslope_t* Slope_Add ", "/// Alocates and fill"
    )
    parts["SLOPE_ALLOC"] = code
    code = ""
    code += sl.section(
        "void P_ReconfigureViaVertexes ", "/// Setup slope via constants."
    )
    for n in ["MakeViaMapthings", "line_SpawnViaMapthingVertexes"]:
        code += sl.function(n)

    parts["SLOPE_BUILD"] = code
    code = ""
    code += Source(engine / "hardware/hw_clip.c").section(
        "typedef struct clipnode_s", "#define RMUL"
    )
    parts["CLIPPER"] = code
    code = ""
    code += "\n#define RMUL (1.6f/1.333333f)\n" + Source(
        engine / "hardware/hw_clip.c"
    ).function("gld_FrustumAngle")
    code += bsp.section("INT32 checkcoord[12][4]", "static boolean R_CheckBBox")
    code += bsp.function("R_IsDebugLine") + bsp.function("R_IsEmptyLine")
    parts["BSP_VISIBILITY"] = code
    code = ""

    # The replay models the camera's map position only, not its height or projection, so the camera-underground
    # rule HWR_AddLine calls (tested by void_wall) is off here.
    code += (
        "\nstatic UINT8 HWR_WallFillsView(seg_t *, sector_t *, sector_t *) { return 0; }"
        "\nstatic void HWR_DrawVoidWall(seg_t *, sector_t *, UINT8) { abort(); }\n"
    )
    for n in ["CheckClip", "HWR_AddLine", "HWR_CheckBBox"]:
        code += hw.function(n)
    code += (
        "\nstatic void HWR_Subsector(size_t);\n"
        + hw.function("HWR_RenderBSPNode")
        + "\n#define _UWP\n"
    )

    solid = Source(engine / "p_spec.c").section(
        "\t\t\tcase 100: // FOF (solid)", "\n\t\t\t\tP_AddFakeFloorsByLine"
    )
    code += (
        "\nstatic ffloortype_e SolidFlags(size_t i){UINT32 ffloorflags{};switch(lines[i].special){\n"
        + solid
        + "\nbreak;}return ffloortype_e(ffloorflags);}\n"
    )
    parts["LINE_BSP_FOF"] = code
    code = ""

    render(
        templates / "title_lightlist.cpp.in",
        {**parts, "PREP": prep},
        output / "title_lightlist.cpp",
    )
