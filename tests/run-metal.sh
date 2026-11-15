#!/usr/bin/env bash
# Headless production driver scenes on the Metal backend (macOS), with Metal's API and shader validation required.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${RR_METAL_BUILD:-"$root/build/metal-tests"}
# Metal's validation is turned on from outside the program. The debug layer stops the program at the first misuse of
# the API; shader validation reports what a shader did wrong with the command buffer, which the test counts.
export MTL_DEBUG_LAYER=1
export MTL_SHADER_VALIDATION=1
# The debug layer's warnings (a state set twice, a buffer bound and never used) stop the program too, not only its errors
export MTL_DEBUG_LAYER_WARNING_MODE=assert
cmake -S "$root/tests/metal-driver" -B "$build" -DCMAKE_BUILD_TYPE=Release ${RR_METAL_CMAKE_ARGS:-}
cmake --build "$build" --parallel 3
"$build/metal-driver"
python3 "$root/tests/metal-controls.py" "$build"
