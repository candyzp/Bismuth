#!/usr/bin/env python3
"""Compile the actual atlas registry and sprite/batch hooks against a fake Cocos/GL API.
This exercises their control flow; it does not replace an iOS SDK/device build.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

def source(name):
    return '\n'.join(line for line in (root / name).read_text().splitlines()
                     if not line.startswith(('#include', '#pragma once')))

hooks = source('src/renderer/hooks.cpp')
start = hooks.index('class $modify(RendererOwnedCCSprite,')
end = hooks.index('\n#endif', start)
atlas_header = source('src/renderer/ios/AtlasInterleave.hpp')
code = '\n'.join([
    '#define GEODE_IS_IOS',
    '#include "ios_atlas_fixture.hpp"',
    '#include "../src/renderer/ios/AtlasDrawPlan.hpp"',
    atlas_header,
    source('src/renderer/ios/AtlasInterleave.cpp'),
    'bool Renderer::prepareGPUOwnedSprite(cocos2d::CCSprite* s) { return enabled && AtlasInterleaveRegistry::shouldSkipTransform(this,s); }',
    hooks[start:end],
    (root / 'tests/ios_atlas_cases.cpp').read_text(),
])
with tempfile.TemporaryDirectory(prefix='bismuth-atlas-tests-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(code)
    subprocess.run(['g++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'tests'), str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
