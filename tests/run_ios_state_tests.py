#!/usr/bin/env python3
"""Run the production state capture/packing/geometry checks with fake sprite state."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
def source(name):
    return '\n'.join(line for line in (root/name).read_text().splitlines()
                     if not line.startswith(('#include','#pragma once')))
header=source('src/renderer/ios/ResolvedStateLayer.hpp').replace('private:', 'public:')
state=source('src/renderer/ios/ResolvedStateLayer.cpp')
active=source('src/renderer/ios/ResolvedStateActive.cpp')
pieces=[
    '#define GEODE_IS_IOS', '#include "ios_state_fixture.hpp"', header,
    state[state.index('namespace {'):state.index('void uploadDirtyRecordSpans(')]+'}',
    state[state.index('ResolvedStateLayer::ObjectState ResolvedStateLayer::captureObjectState('):state.index('bool ResolvedStateLayer::init(')],
    state[state.index('bool ResolvedStateLayer::canDrawSprite('):state.index('void ResolvedStateLayer::setGPUOwnedSprites(')],
    'namespace { ResolvedStateLayer* g_currentResolvedState=nullptr; }',
    'void ResolvedStateLayer::destroyTextures() {}',
    active[active.index('ResolvedStateLayer::ResolvedStateLayer()'):active.index('void ResolvedStateLayer::ensureEventOwnership()')],
    (root/'tests/ios_state_cases.cpp').read_text(),
]
with tempfile.TemporaryDirectory(prefix='bismuth-state-tests-') as directory:
    path=Path(directory); (path/'test.cpp').write_text('\n'.join(pieces))
    subprocess.run(['g++','-std=c++23','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-I',str(root/'tests'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
