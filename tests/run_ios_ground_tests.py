#!/usr/bin/env python3
"""Compile production ground ownership, submission and hook code with a fake GL API."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
def source(name):
    return '\n'.join(line for line in (root / name).read_text().splitlines()
                     if not line.startswith(('#include', '#pragma once')))
code = '\n'.join([
    '#define GEODE_IS_IOS', '#include "ios_ground_fixture.hpp"',
    source('src/renderer/ios/GroundOwnership.hpp'),
    source('src/renderer/ios/GroundGPU.cpp'),
    (root / 'tests/ios_ground_cases.cpp').read_text(),
])
with tempfile.TemporaryDirectory(prefix='bismuth-ground-tests-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'tests'), str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
