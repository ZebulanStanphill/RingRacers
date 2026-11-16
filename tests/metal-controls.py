#!/usr/bin/env python3
"""Run WARP's code-under-test negative controls against the Metal harness (see vulkan-controls.py), and, before
them, the runs that check what a failed Metal command buffer is to the driver."""
import pathlib
import subprocess
import sys

build = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(__file__).resolve().parent.parent
source = root / "src/hardware/r_d3d12/shaders.hlsl"
exe = build / "metal-driver"


def reject(scene):
    result = subprocess.run([str(exe), "--negative-control", scene],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=120)
    print(result.stdout, end="")
    if result.returncode != 1 or f"NEGATIVE_ORACLE_REJECTED: {scene}" not in result.stdout:
        raise RuntimeError(f"{scene}: expected pixel oracle rejection, exit {result.returncode}")


def compile_source(path):
    subprocess.run(["cmake", "-S", str(root / "tests/metal-driver"), "-B", str(build),
                    f"-DRR_VULKAN_SHADER_SOURCE={path}"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "metal-driver",
                    "--parallel", "3"], check=True)


def command_buffer_error(code, reason, *where):
    """A command buffer that Metal fails with this MTLCommandBufferError code (mtl12 is told to treat the next one as
    failed) must reach the driver as a removed device with this reason, and the frame after it must stop the game:
    the test's I_Error exits with 1. where: "null-view" for the command buffer that writes a null view's texture
    instead of one of the driver's."""
    name = " ".join([str(code), *where])
    result = subprocess.run([str(exe), "--metal-command-buffer-error", str(code), *where],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=120)
    print(result.stdout, end="")
    if (result.returncode != 1 or f"METAL_FAILURE_REACHED_DRIVER: {name} as {reason}" not in result.stdout
            or f"device stopped working ({reason})" not in result.stdout):
        raise RuntimeError(f"command buffer error {name}: expected the game to stop with {reason}, exit {result.returncode}")


# A timeout, a page fault, out of memory, and the device removed
for code, reason in ((2, "0x887A0006"), (3, "0x887A0007"), (8, "0x887A0007"), (11, "0x887A0005")):
    command_buffer_error(code, reason)
command_buffer_error(2, "0x887A0006", "null-view")
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
