#!/usr/bin/env python3
"""Fails if a tracked file uses a name the VibeRant driver had when it was only a Direct3D 12 driver.

usage: check-retired-names.py   (from anywhere inside the repository)

The driver's switches, folder, namespace, and functions were renamed after VibeRant. A branch started before
that can bring an old name back, and `#ifdef HAVE_D3D12` would then compile, and quietly leave its code out.
The names, old to new:

  HAVE_D3D12            HAVE_VIBERANT (the driver, with any backend)
  HAVE_D3D12_NATIVE     HAVE_VIBERANT_D3D
  HAVE_VULKAN           HAVE_VIBERANT_VULKAN
  HAVE_METAL            HAVE_VIBERANT_METAL
  SRB2_CONFIG_D3D12     SRB2_CONFIG_VIBERANT_D3D (and _VULKAN, _METAL alike)
  HWR_D3D12()           HWR_VibeRant()
  hwr_d3d12             hwr_viberant
  SCR_RendererD3D12()   SCR_RendererVibeRant()
  rr_d3d12              rr_viberant (the namespace)
  r_d3d12               r_viberant (the folder, r_viberant.cpp, r_viberant.h)
  d3d12_core.cpp, ...   viberant_core.cpp, ... (d3d12_present*.cpp: d3d_present_uwp.cpp, d3d_present_win32.cpp)
  D3D_Init, D3D12_...   VibeRant_Init, VibeRant_...
  D3D12_LOG_ONCE        VIBERANT_LOG_ONCE
  HWR_UWPSetEncoreMap   HWR_SetEncoreMap (it never was only for UWP)
"""
import os
import re
import subprocess
import sys

SKIP = ('thirdparty/', 'libs/', 'uwp/pgo/', 'uwp/patches/', 'tests/check-retired-names.py')
B, E = r'(?<![A-Za-z0-9_])', r'(?![A-Za-z0-9_])'
RETIRED = re.compile(B + r'(?:(?:-D|/D)?(?:HAVE_D3D12(?:_NATIVE)?|HAVE_VULKAN|HAVE_METAL)'
                     r'|(?:-D)?SRB2_CONFIG_(?:D3D12|VULKAN|METAL)(?:_DEFAULT)?'
                     r'|HWR_D3D12|hwr_d3d12|SCR_RendererD3D12|HWR_UWPSetEncoreMap|rr_d3d12\w*|r_d3d12\w*|D3D12_LOG_ONCE'
                     r'|d3d12_(?:core|pipeline|prewarm|upload|present|arenas|captures|models|palette|placement'
                     r'|screeneffect|screens)(?:_win32)?\.(?:cpp|h|inc)'
                     r'|D3D(?:12)?_(?:[A-Z][a-z][A-Za-z0-9]*|GClipRect|##))' + E)

root = subprocess.run(['git', 'rev-parse', '--show-toplevel'], check=True, capture_output=True, text=True).stdout.strip()
files = subprocess.run(['git', '-C', root, 'ls-files', '-z'], check=True, capture_output=True, text=True).stdout.split('\0')
found = 0
for path in files:
    full = os.path.join(root, path)
    if not path or path.startswith(SKIP) or os.path.islink(full) or not os.path.isfile(full):
        continue
    data = open(full, 'rb').read()
    if b'\0' in data[:8192]:
        continue
    for number, line in enumerate(data.decode('utf-8', 'replace').split('\n'), 1):
        for match in RETIRED.finditer(line):
            print(f'{path}:{number}: {match.group(0)}')
            found += 1
if found:
    sys.exit(f'{found} uses of retired names; see the table at the top of tests/check-retired-names.py')
print('No retired names')
