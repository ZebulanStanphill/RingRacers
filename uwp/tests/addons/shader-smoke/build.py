#!/usr/bin/env python3
"""Build a reproducible shader-only PK3, or validate the shipped copy."""

import argparse
import io
import json
from pathlib import Path
import zipfile

HERE = Path(__file__).resolve().parent
PACKAGE = HERE / "D_ShaderSmoke_v1.pk3"
FILES = (
    "SHADERS",
    "Shaders/sh_smoke_world.vert",
    "Shaders/sh_smoke_world.frag",
    "Shaders/sh_smoke_water.frag",
)


def package_bytes():
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        for name in FILES:
            source = "definitions.txt" if name == "SHADERS" else name
            data = (HERE / "source" / source).read_text(encoding="ascii")
            data = data.replace("\r\n", "\n").encode("ascii")
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--header", type=Path, help="Extract the packaged sources for native loader tests")
    args = parser.parse_args()
    expected = package_bytes()
    if args.check:
        if not PACKAGE.exists() or PACKAGE.read_bytes() != expected:
            parser.error("PK3 differs from source; run build.py to rebuild it")
    else:
        PACKAGE.write_bytes(expected)
    with zipfile.ZipFile(PACKAGE) as archive:
        if archive.namelist() != list(FILES) or archive.testzip() is not None:
            parser.error("Invalid PK3 entries or CRCs")
        if args.header:
            lines = ["struct SmokeFile { const char *name, *source; };", "const SmokeFile smokeFiles[] = {"]
            for name in FILES:
                source = archive.read(name).decode("ascii")
                lines.append("    {" + json.dumps(name) + ", " + json.dumps(source) + "},")
            lines.append("};")
            args.header.parent.mkdir(parents=True, exist_ok=True)
            args.header.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"{'Verified' if args.check else 'Built'} {PACKAGE.name}: {len(expected)} bytes, four shader-only entries")


if __name__ == "__main__":
    main()
