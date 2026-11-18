#!/usr/bin/env python3
"""Fails if a fork change inside an upstream source file has no [ZE] or [UWP] marker comment.

usage: check-change-markers.py [--base <commit>]   (from anywhere inside the repository)

AGENTS.md ("Staying rebaseable") asks for a comment starting `[ZE]:` or `[UWP]:` at every change the fork makes inside
a file upstream also has, so that each one can be found and explained when the fork is rebased onto a newer upstream
release. This compares the committed HEAD with the upstream commit the fork is based on: by default the merge base of
HEAD and `upstream/maintenance-2.4`, falling back to FETCH_HEAD (CI fetches upstream's branch without naming a remote).

Only files that exist on both sides count (new files are the fork's own), and only C, C++, CMake, and `config.h.in`
sources. A changed place ("hunk", from `git diff -U0`) counts as marked when a marker appears in its changed lines or
up to three lines above them; a hunk that only removes upstream lines counts as marked when a marker is on the line at
the removal or up to three lines above it. Hunks come from git's histogram diff, named explicitly: the default diff
algorithm (or a user's `diff.algorithm` setting) can split and place hunks differently.
"""
import os
import re
import subprocess
import sys

NEAR = 3
MARK = re.compile(r'\[(ZE|UWP)\]')
SOURCE = re.compile(r'\.(c|cpp|h|hpp)$|CMakeLists\.txt$|\.cmake$|config\.h\.in$')
HUNK = re.compile(r'@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@')


def git(*args, check=True):
    result = subprocess.run(['git', *args], capture_output=True, text=True, errors='replace')
    if check and result.returncode:
        sys.exit(f'git {" ".join(args)} failed: {result.stderr.strip()}')
    return result


def find_base():
    if '--base' in sys.argv:
        return sys.argv[sys.argv.index('--base') + 1]
    for ref in ('upstream/maintenance-2.4', 'FETCH_HEAD'):
        result = git('merge-base', ref, 'HEAD', check=False)
        if result.returncode == 0:
            return result.stdout.strip()
    sys.exit('No upstream commit to compare with: add the upstream remote and fetch maintenance-2.4, '
             'or pass --base <commit>.')


def main():
    os.chdir(git('rev-parse', '--show-toplevel').stdout.strip())
    base = find_base()
    files = [f for f in git('diff', '--diff-filter=M', '--name-only', base, 'HEAD').stdout.split('\n')
             if SOURCE.search(f)]
    unmarked = total = 0
    for path in files:
        new = git('show', f'HEAD:{path}').stdout.split('\n')
        for line in git('diff', '-U0', '--histogram', '--indent-heuristic', '--no-ext-diff', base, 'HEAD', '--', path).stdout.split('\n'):
            match = HUNK.match(line)
            if not match:
                continue
            start, count = int(match.group(1)), int(match.group(2) or 1)
            total += 1
            if count == 0:
                lo, hi = max(0, start - NEAR), min(len(new), start + 1)
            else:
                lo, hi = max(0, start - 1 - NEAR), min(len(new), start - 1 + count)
            if not any(MARK.search(text) for text in new[lo:hi]):
                unmarked += 1
                shown = new[min(max(start, 1), len(new)) - 1].strip()[:90]
                print(f'{path}:{start}: change without a [ZE] or [UWP] marker: {shown}')
    print(f'{unmarked} of {total} changed places in {len(files)} upstream files lack a marker '
          f'(compared with upstream {base[:9]})')
    if unmarked:
        print('Add a comment starting [ZE]: (or [UWP]: for Xbox-only code) in or just above each place: see AGENTS.md, '
              '"Staying rebaseable".')
    return 1 if unmarked else 0


if __name__ == '__main__':
    sys.exit(main())
