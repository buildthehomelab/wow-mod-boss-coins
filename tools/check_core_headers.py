#!/usr/bin/env python3
"""Compile the wallet module against real core headers, without linking a worldserver."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--boost', type=Path)
    parser.add_argument('--openssl-include', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    core = args.core.resolve()
    if not (core / 'src/server/game/Instances/InstanceScript.h').is_file():
        parser.error('--core must be a complete AzerothCore source checkout')
    dirs = set()
    for base in ('src/common', 'src/server/game', 'src/server/shared', 'src/server/database',
                 'deps/fmt', 'deps/g3dlite', 'deps/recastnavigation'):
        for pattern in ('*.h', '*.hpp'):
            for path in (core / base).rglob(pattern):
                for parent in (path.parent, *path.parent.parents):
                    if parent == core:
                        break
                    if core in parent.parents:
                        dirs.add(parent)
    ordered = sorted(dirs, key=lambda path: (0 if 'src' in path.relative_to(core).parts[:1] else 1, str(path)))
    ordered.extend(path.resolve() for path in (args.boost, args.openssl_include) if path)
    module = Path(__file__).resolve().parents[1]
    command = [args.compiler, '-std=c++20', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-deprecated-declarations']
    command.extend('-I' + str(path) for path in ordered)
    command.extend(str(module / 'src' / name) for name in sorted(path.name for path in (module / 'src').glob('*.cpp')))
    return subprocess.run(command, check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
