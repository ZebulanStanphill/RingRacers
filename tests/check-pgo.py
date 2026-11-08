#!/usr/bin/env python3
"""Run the UWP PGO lifecycle against SDL/compiler-rt mocks, without an SDK."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build' / 'pgo-lifecycle-test'
build.mkdir(parents=True, exist_ok=True)
(build / 'SDL.h').write_text('''#pragma once
#define SDLCALL
#define SDL_APP_WILLENTERBACKGROUND 0x103
struct SDL_Event { unsigned type; };
using SDL_EventFilter = int (*)(void *, SDL_Event *);
void SDL_AddEventWatch(SDL_EventFilter, void *);
''')
(build / 'main.cpp').write_text(r'''
#include <cassert>
#include <cstdarg>
#include <string>
#include <vector>
#include "SDL.h"
#include "i_system.h"
static std::string filename;
static std::vector<std::string> writes, directories, logs;
static int resets, result, watches;
static SDL_EventFilter watch;
void SDL_AddEventWatch(SDL_EventFilter callback, void *) { watch = callback; ++watches; }
extern "C" {
void __llvm_profile_set_filename(const char *name) { filename = name; }
int __llvm_profile_write_file() { writes.push_back(filename); return result; }
void __llvm_profile_reset_counters() { ++resets; }
INT32 I_mkdir(const char *name, INT32) { directories.emplace_back(name); return 1; }
void I_OutputMsg(const char *fmt, ...) {
    char line[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap); va_end(ap); logs.emplace_back(line);
}
}
int main() {
    I_UWPPgoDump(); assert(writes.empty());
    I_UWPPgoInit(); I_UWPPgoInit();
    assert(watches == 1 && directories.size() == 2);
    assert(directories.back() == "E:\\ringracers\\pgo");
    const auto first = filename;
    assert(first.find("E:\\ringracers\\pgo\\ringracers-") == 0);
    assert(first.find('%') == std::string::npos);
    assert(first.substr(first.size()-8) == ".profraw");
    I_UWPPgoDump(); assert(writes.back() == first && resets == 1 && filename != first);
    const auto second = filename;
    result = -1; I_UWPPgoDump();
    assert(resets == 1 && filename == second && logs.back().find("FAILED") != std::string::npos);
    result = 0; SDL_Event event{SDL_APP_WILLENTERBACKGROUND}; watch(nullptr, &event);
    assert(writes.back() == second && resets == 2 && filename != second);
    event.type = 0; watch(nullptr, &event); assert(writes.size() == 3);
    assert(logs.back().find("saved") != std::string::npos);
}
''')
compiler = os.environ.get('CXX', 'clang++')
subprocess.run([compiler, '-std=c++17', '-D_UWP', '-DSRB2_PGO_GENERATE',
                '-fsanitize=address,undefined', '-g', '-I' + str(build),
                '-I' + str(root / 'src'), str(build / 'main.cpp'),
                str(root / 'src/sdl/i_uwppgo.cpp'), '-o', str(build / 'check-pgo')], check=True)
subprocess.run([str(build / 'check-pgo')], check=True, cwd=build)
print('PGO lifecycle: passed (ASan/UBSan)')
