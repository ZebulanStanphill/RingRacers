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
    original = hw.function("HWR_AddLine")
    for negative in (False, True):
        addline = original
        if negative:
            # Remove just the seven-line UWP preparation block. Unfixed checkouts are
            # allowed through extraction so the ordinary replay fails on their behavior.
            start = addline.find(
                "#ifdef _UWP\n\t// FOF sides may sample the adjacent sector"
            )
            if start != -1:
                end = addline.index("#endif", start) + len("#endif")
                addline = addline[:start] + addline[end:]

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


        for n in ["CheckClip", "HWR_AddLine", "HWR_CheckBBox"]:
            code += addline if n == "HWR_AddLine" else hw.function(n)
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
            output
            / ("title_lightlist_negative.cpp" if negative else "title_lightlist.cpp"),
        )
