"""The renderers a sweep can play with, and everything that differs between them, in one table.

Adding a renderer (VibeRant Metal, say) is a row here, a build job for its platform in the workflow, and its setup steps
in the capture job. Nothing else knows the names: the planner builds the matrix from this table, the capture script takes
its flags and log assertions from it, and the report goes through whatever renderers the run captured.
"""

# label: for reports. os: the runner. build: the name of the build job whose artifact it plays. flags: what picks the
# renderer. requires: lines the game's log must contain, or the renderer that was asked for isn't the one that ran.
# forbids: lines that mean another one did. errors: patterns that fail a map, besides the ones every renderer has (see
# capture.py), for what only this renderer's debugging layer says. seconds: wall seconds of one map (five frames, the long schedule) on a hosted
# runner by frame size, from the hosted-runner probes of 2026-11-15; the planner uses them to split a sweep into shards
# that finish well inside the job limit. Re-measure when a renderer gets slower or faster (the manifest of a sweep has
# every map's seconds).
RENDERERS = {
    'software': dict(
        label='Software', os='ubuntu-24.04', build='linux', flags=['-software'],
        requires=[], forbids=['VID_StartupOpenGL'], errors=[],
        seconds={'320x200': 9, '640x400': 14, '1280x720': 25}),
    'gl': dict(
        label='Legacy GL', os='ubuntu-24.04', build='linux', flags=['-opengl'],
        requires=['VID_StartupOpenGL'], forbids=['VibeRant backend'], errors=[],
        seconds={'320x200': 22, '640x400': 44, '1280x720': 68}),
    'vulkan': dict(
        label='VibeRant Vulkan', os='ubuntu-24.04', build='linux', flags=['-vulkan', '-vkdebug'],
        requires=['VibeRant backend: Vulkan', 'Vulkan: device: llvmpipe', 'validation on'], forbids=[], errors=[],
        seconds={'320x200': 93, '640x400': 118, '1280x720': 184}),
    'd3d': dict(
        label='VibeRant D3D', os='windows-2022', build='windows', flags=['-d3d12', '-warp', '-d3ddebug'],
        requires=['VibeRant backend: D3D12', 'D3D12 debug layer enabled', 'using WARP for testing',
                  "adapter 'Microsoft Basic Render Driver'"], forbids=[], errors=[],
        seconds={'320x200': 65, '640x400': 111, '1280x720': 290}),
}
SIZES = ['320x200', '640x400', '1280x720']
DEFAULT_SIZE = '640x400'
# Which renderers are compared with each other, and always as information only
CROSS = [('vulkan', 'd3d')]
