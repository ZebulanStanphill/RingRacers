#!/usr/bin/env bash
# Headless production driver scenes, with Vulkan validation required.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${RR_VULKAN_BUILD:-"$root/build/vulkan-tests"}
cmake -S "$root/tests/vulkan-driver" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel 3
"$build/vulkan-driver"
python3 "$root/tests/vulkan-controls.py" "$build"
