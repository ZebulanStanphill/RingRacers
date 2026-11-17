#!/usr/bin/env python3
"""Run WARP's code-under-test negative controls against the Vulkan harness."""
import pathlib
import subprocess
import sys

build = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(__file__).resolve().parent.parent
source = root / "src/hardware/r_viberant/shaders.hlsl"
exe = build / "vulkan-driver"


def reject(scene):
    result = subprocess.run([str(exe), "--negative-control", scene],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=120)
    print(result.stdout, end="")
    if result.returncode != 1 or f"NEGATIVE_ORACLE_REJECTED: {scene}" not in result.stdout:
        raise RuntimeError(f"{scene}: expected pixel oracle rejection, exit {result.returncode}")


def compile_source(path):
    subprocess.run(["cmake", "-S", str(root / "tests/vulkan-driver"), "-B", str(build),
                    f"-DRR_VULKAN_SHADER_SOURCE={path}"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "vulkan-driver",
                    "--parallel", "3"], check=True)


reject("custom-shader-nocustom")
controls = [
    ("water-refraction", "WaterBackgroundOffset(i.pos.w, u_leveltime)",
     "WaterBackgroundOffset(1.0 / i.pos.w, u_leveltime)"),
    ("fog-palette", "FogColormap(i.pos.w, u_fog_lightnum",
     "FogColormap(1.0 / i.pos.w, u_fog_lightnum"),
    ("brightmap-palette", "brightmap_mix > 0.0 ? 32 : ", ""),
    ("surface-light", "u_light_plane, u_light_view_ratio);", "1, u_light_view_ratio);"),
    ("surface-light-semibright", "u_light_plane == 2", "u_light_plane == 3"),
]
production = source.read_text()
try:
    for scene, good, bad in controls:
        if good not in production:
            raise RuntimeError(f"Cannot inject {scene}")
        mutant = build / "negative-controls" / scene / "shaders.hlsl"
        mutant.parent.mkdir(parents=True, exist_ok=True)
        mutant.write_text(production.replace(good, bad))
        compile_source(mutant)
        reject(scene)
finally:
    # Leave the build usable with production shaders even when a control fails.
    compile_source(source)
