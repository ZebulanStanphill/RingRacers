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
# The driver's thread is the game's main loop, which is C++ and has no autorelease pool, so mtl12 has to bring one to
# every call that can autorelease: an object autoreleased without a pool stays for good. The Objective-C runtime reports
# each such object when asked to, by thread, and the test says which thread is its own. (Metal's own threads report
# thousands under validation, which is the system's business.)
log="$build/metal-driver.log"
if ! OBJC_DEBUG_MISSING_POOLS=YES "$build/metal-driver" > "$log" 2>&1; then
    grep -v "MISSING POOLS" "$log"
    exit 1
fi
grep -v "MISSING POOLS" "$log"
thread=$(sed -n 's/^Metal driver test: main thread \(0x[0-9a-f]*\)$/\1/p' "$log")
if [ -z "$thread" ]; then
    echo "The test didn't say which thread is its own" >&2
    exit 1
fi
if grep "MISSING POOLS: ($thread)" "$log" | head -n 20 | grep .; then
    echo "The driver's thread autoreleased objects with no autorelease pool in place" >&2
    exit 1
fi
echo "The driver's thread autoreleased nothing outside a pool ($(grep -c "MISSING POOLS" "$log") reports from Metal's own threads)"
python3 "$root/tests/metal-controls.py" "$build"
