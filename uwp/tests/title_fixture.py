"""Expand compact title-map inputs into C++ initialization at configure time."""

import json
import struct
from pathlib import Path


def generate_fixture(fixtures, out):
    m = json.loads((fixtures / "title-map.json").read_text())
    b = (fixtures / "title-znodes.bin").read_bytes()
    p = 4
    orig, new = struct.unpack_from("<II", b, p)
    p += 8
    verts = [(v["x"], v["y"]) for v in m["vertex"]]
    for i in range(new):
        x, y = struct.unpack_from("<ii", b, p)
        p += 8
        verts.append((x / 65536, y / 65536))
    numsubs = struct.unpack_from("<I", b, p)[0]
    p += 4
    counts = list(struct.unpack_from("<" + "I" * numsubs, b, p))
    p += 4 * numsubs
    numsegs = struct.unpack_from("<I", b, p)[0]
    p += 4
    ss = []
    sg = []
    for count in counts:
        first = len(sg)
        for j in range(count):
            v1, partner, line, side = struct.unpack_from("<IIHB", b, p)
            p += 11
            sg.append([v1, 0, line, side])
        for j in range(count):
            sg[first + j][1] = sg[first + (j + 1) % count][0]
        ss.append((first, count))
    numnds = struct.unpack_from("<I", b, p)[0]
    p += 4
    nds = []
    for i in range(numnds):
        vals = struct.unpack_from("<12hII", b, p)
        p += 32
        nds.append(vals)
    assert p == len(b), (p, len(b))
    flatnames = list(
        dict.fromkeys(
            ["F_SKY1"]
            + [s[k] for s in m["sector"] for k in ["texturefloor", "textureceiling"]]
        )
    )
    flatid = lambda n: flatnames.index(n) + 1
    code = [
        "static void InitFixture(){",
        "fog.rgba=fog.fadergba=0x19000000;fog.fadeend=31;fog.flags=CMF_FOG;cv_glshaders.value=1;",
        "for(auto &t:tex){t.width=t.height=64;}",
    ]
    for i, (x, y) in enumerate(verts):
        code += [
            f"mapVerts[{i}].x=FLOAT_TO_FIXED({x});mapVerts[{i}].y=FLOAT_TO_FIXED({y});pverts[{i}].x={x};pverts[{i}].y={y};"
        ]
    for i, s in enumerate(m["sector"]):
        tags = ([s["id"]] if "id" in s else []) + [
            int(t) for t in s.get("moreids", "").split()
        ]
        code += [
            f'mapSectors[{i}].floorheight={s["heightfloor"]}*FRACUNIT;mapSectors[{i}].ceilingheight={s["heightceiling"]}*FRACUNIT;mapSectors[{i}].lightlevel={s["lightlevel"]};mapSectors[{i}].heightsec=-1;mapSectors[{i}].floorlightsec=-1;mapSectors[{i}].ceilinglightsec=-1;mapSectors[{i}].moved=true;',
            f'mapSectors[{i}].floorpic={flatid(s["texturefloor"])};mapSectors[{i}].ceilingpic={flatid(s["textureceiling"])};',
        ]
        if tags:
            code += [
                f'static mtag_t tags{i}[]={{{",".join(map(str,tags))}}};mapSectors[{i}].tags={{tags{i},{len(tags)}}};'
            ]
    code += ["mapSectors[61].extra_colormap=&fog;"]
    for i, s in enumerate(m["sidedef"]):
        code += [f'mapSides[{i}].sector=mapSectors+{s["sector"]};']
        for k, key in [
            ("toptexture", "texturetop"),
            ("bottomtexture", "texturebottom"),
            ("midtexture", "texturemiddle"),
        ]:
            code += [f'mapSides[{i}].{k}={int(s.get(key,"-")!="-")};']
    for i, l in enumerate(m["linedef"]):
        code += [
            f'mapLines[{i}].v1=mapVerts+{l["v1"]};mapLines[{i}].v2=mapVerts+{l["v2"]};mapLines[{i}].sidenum[0]={l["sidefront"]};mapLines[{i}].sidenum[1]={l.get("sideback",65535)};mapLines[{i}].frontsector=mapSides[{l["sidefront"]}].sector;mapLines[{i}].alpha=FRACUNIT;'
        ]
        if "sideback" in l:
            code += [f'mapLines[{i}].backsector=mapSides[{l["sideback"]}].sector;']
        for arg in range(10):
            code += [f'mapLines[{i}].args[{arg}]={l.get("arg"+str(arg),0)};']
        for k, v in l.items():
            if (
                k
                in [
                    "twosided",
                    "blocking",
                    "dontpegtop",
                    "dontpegbottom",
                    "noskew",
                    "skewtd",
                    "midpeg",
                    "wrapmidtex",
                ]
                and v
            ):
                code += [
                    f"mapLines[{i}].flags|=ML_"
                    + {
                        "twosided": "TWOSIDED",
                        "blocking": "IMPASSABLE",
                        "dontpegtop": "DONTPEGTOP",
                        "dontpegbottom": "DONTPEGBOTTOM",
                        "noskew": "NOSKEW",
                        "skewtd": "SKEWTD",
                        "midpeg": "MIDPEG",
                        "wrapmidtex": "WRAPMIDTEX",
                    }[k]
                    + ";"
                ]
            if k in ["special"]:
                code += [f"mapLines[{i}].special={v};"]

    # The title's only slope-spawning line special is704, and no UDMF vertex z is set.
    assert not any(
        any(k in v for k in ["zfloor", "zceiling", "floorz", "ceilingz"])
        for v in m["vertex"]
    )
    assert {l.get("special", 0) for l in m["linedef"]} <= {0, 7, 100, 202, 259, 704}
    slopeLines = [(i, l) for i, l in enumerate(m["linedef"]) if l.get("special") == 704]
    assert len(slopeLines) == 72
    assert {l.get("arg0", 0) for i, l in slopeLines} == {0, 1}
    assert all(l.get("arg4", 0) == 0 for i, l in slopeLines)
    thingById = {t.get("id", 0): t for t in m["thing"] if t.get("type") == 750}
    assert len(thingById) == 89
    assert all(t.get("thingarg0", 0) == 1 for t in thingById.values())
    assert all(t.get("type") in [1, 750, 770] for t in m["thing"])
    assert not any(sec.get("action", 0) for sec in m["sector"])
    for i, t in enumerate(m["thing"]):
        code += [
            f'fixtureThings[{i}].type={t["type"]};fixtureThings[{i}].tid={t.get("id",0)};fixtureThings[{i}].x={int(t["x"])};fixtureThings[{i}].y={int(t["y"])};fixtureThings[{i}].z={int(t.get("height",0))};fixtureThings[{i}].thing_args[0]={t.get("thingarg0",0)};'
        ]
    code += [f'nummapthings={len(m["thing"])};']

    # Match P_GroupLines soundorg: midpoint of integer map-coordinate bbox, truncated toward zero.
    for sector in range(len(m["sector"])):
        points = []
        for l in m["linedef"]:
            refs = [l["sidefront"]] + ([l["sideback"]] if "sideback" in l else [])
            if any(m["sidedef"][ref]["sector"] == sector for ref in refs):
                points += [m["vertex"][l["v1"]], m["vertex"][l["v2"]]]
        assert points
        sx = int((min(v["x"] for v in points) + max(v["x"] for v in points)) / 2)
        sy = int((min(v["y"] for v in points) + max(v["y"] for v in points)) / 2)
        code += [
            f"mapSectors[{sector}].soundorg.x={sx}*FRACUNIT;mapSectors[{sector}].soundorg.y={sy}*FRACUNIT;mapSectors[{sector}].soundorg.z=mapSectors[{sector}].floorheight;"
        ]

    for i, l in slopeLines:
        for a in [1, 2, 3]:
            assert l.get("arg" + str(a), 0) in thingById
        code += [f"line_SpawnViaMapthingVertexes({i},false);"]
    code += ["assert(slopecount==72);"]

    for i, l in enumerate(m["linedef"]):
        if l.get("special") not in [100, 202, 259]:
            continue
        ctrl = m["sidedef"][l["sidefront"]]["sector"]
        flags = (
            "SolidFlags(" + str(i) + ")"
            if l["special"] == 100
            else (
                "ffloortype_e(" + str(l["arg3"]) + ")"
                if l["special"] == 259
                else "ffloortype_e(FOF_EXISTS|FOF_RENDERALL|FOF_FOG|FOF_INVERTPLANES|FOF_INVERTSIDES|FOF_CUTEXTRA|FOF_EXTRA|FOF_DOUBLESHADOW|FOF_CUTSPRITES)"
            )
        )
        for target, sec in enumerate(m["sector"]):
            tags = ([sec["id"]] if "id" in sec else []) + [
                int(t) for t in sec.get("moreids", "").split()
            ]
            if l["arg0"] in tags and target != ctrl:
                code += [f"AppendFOF({target},{ctrl},{i},{flags});"]
    for i, (v1, v2, l, side) in enumerate(sg):
        code += [
            f"mapSegs[{i}].v1=mapVerts+{v1};mapSegs[{i}].v2=mapVerts+{v2};mapSegs[{i}].pv1=pverts+{v1};mapSegs[{i}].pv2=pverts+{v2};mapSegs[{i}].glseg={int(l==65535)};mapSegs[{i}].side={side};"
        ]
        if l != 65535:
            a = m["sidedef"][m["linedef"][l]["sidefront" if side == 0 else "sideback"]][
                "sector"
            ]
            bb = m["linedef"][l].get("sideback" if side == 0 else "sidefront")
            back = m["sidedef"][bb]["sector"] if bb is not None else -1
            code += [
                f'mapSegs[{i}].linedef=mapLines+{l};mapSegs[{i}].sidedef=mapSides+{m["linedef"][l]["sidefront" if side==0 else "sideback"]};mapSegs[{i}].frontsector=mapSectors+{a};',
                f"mapSegs[{i}].flength=std::hypot({verts[v1][0]-verts[v2][0]},{verts[v1][1]-verts[v2][1]});",
            ]
            if back != -1:
                code += [f"mapSegs[{i}].backsector=mapSectors+{back};"]
    for i, (first, count) in enumerate(ss):
        valid = next(k for k in range(first, first + count) if sg[k][2] != 65535)
        code += [
            f"mapSubs[{i}].firstline={first};mapSubs[{i}].numlines={count};mapSubs[{i}].sector=mapSegs[{valid}].frontsector;"
        ]
    for i, v in enumerate(nds):
        code += [
            f"mapNodes[{i}].x={v[0]}*FRACUNIT;mapNodes[{i}].y={v[1]}*FRACUNIT;mapNodes[{i}].dx={v[2]}*FRACUNIT;mapNodes[{i}].dy={v[3]}*FRACUNIT;"
        ]
        for j in [0, 1]:
            for k in range(4):
                code += [f"mapNodes[{i}].bbox[{j}][{k}]={v[4+j*4+k]}*FRACUNIT;"]
            c = v[12 + j]
            c = (c & 65535) | ((c >> 16) & 0xC000)
            code += [f"mapNodes[{i}].children[{j}]={c};"]
    code += [
        "}",
        f'static constexpr int fixtureRoot={numnds-1};static constexpr int fixtureSectors={len(m["sector"])};',
    ]
    body = code[1:-2]
    chunks = [body[i : i + 350] for i in range(0, len(body), 350)]
    small = []
    for i, chunk in enumerate(chunks):
        small += [f"static void InitFixturePart{i}(){{", *chunk, "}"]
    small += [
        "static void InitFixture(){",
        *[f"InitFixturePart{i}();" for i in range(len(chunks))],
        "}",
        code[-1],
    ]
    (out / "title-fixture.h").write_text("\n".join(small))
