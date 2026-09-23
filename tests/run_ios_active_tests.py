#!/usr/bin/env python3
"""Compile the production iOS active-set tracker and exercise stock lifecycle edges."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

def source(name):
    return '\n'.join(
        line for line in (root / name).read_text().splitlines()
        if not line.startswith(('#include', '#pragma once'))
    )

header = source('src/renderer/ios/ResolvedStateLayer.hpp').replace('private:', 'public:')
active = source('src/renderer/ios/ResolvedStateActive.cpp')

fixture = r'''
#define GEODE_IS_IOS
#include "ios_state_fixture.hpp"
namespace geode { namespace prelude {
namespace log { template<class... T> void info(T&&...) {} }
}}
'''

cases = r'''
void ResolvedStateLayer::destroyTextures() {}

int main() {
    GameObject object;
    object.parent = &object;

    ResolvedStateLayer state;
    state.objects.push_back({});
    state.objects[0].object = &object;
    state.objects[0].safety = ResolvedStateLayer::SafetyClass::DynamicSafe;
    state.objects[0].firstSprite = 0;
    state.objects[0].spriteCount = 1;

    state.sprites.push_back({});
    state.sprites[0].sprite = &object;
    state.sprites[0].objectIndex = 0;
    state.spriteIndexByPointer.emplace(&object, 0);

    // setGPUOwnedSprites() seeds these complete ownership vectors before the
    // event tracker compiles them into masks.
    state.activeObjectIndices = {0};
    state.activeSpriteIndices = {0};
    assert(state.isSpriteActive(&object)); // conservative pre-seed behavior

    state.ensureEventOwnership();
    state.reseedActiveFromStock();
    assert(state.eventOwnershipReady);
    assert(state.isSpriteActive(&object));
    assert(state.getStats().activeGPUObjects == 1);
    assert(state.getStats().activeGPUSprites == 1);

    // Stock deactivation remains active until end-of-render cleanup so a real
    // GPU submission can upload the final hidden state.
    object.parent = nullptr;
    state.onObjectDeactivated(&object);
    assert(state.isSpriteActive(&object));
    assert(state.pendingDeactivateIndices.size() == 1);
    state.finishEventFrame();
    assert(!state.isSpriteActive(&object));
    assert(state.getStats().activeGPUObjects == 0);
    assert(state.getStats().activeGPUSprites == 0);

    // Reactivation restores ownership immediately.
    object.parent = &object;
    state.onObjectActivated(&object);
    assert(state.isSpriteActive(&object));

    // Deactivate + reactivate in one frame cancels the pending retirement.
    object.parent = nullptr;
    state.onObjectDeactivated(&object);
    object.parent = &object;
    state.onObjectActivated(&object);
    state.finishEventFrame();
    assert(state.isSpriteActive(&object));
    assert(state.pendingDeactivateIndices.empty());

    std::cout << "PASS: production active-set seed, deactivate retirement, and same-frame reactivation\n";
}
'''

code = '\n'.join([fixture, header, active, cases])
with tempfile.TemporaryDirectory(prefix='bismuth-active-tests-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(code)
    subprocess.run([
        'g++', '-std=c++23', '-O1', '-g', '-Wall', '-Wextra',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I', str(root / 'tests'), str(path / 'test.cpp'), '-o', str(path / 'test')
    ], check=True)
    subprocess.run([str(path / 'test')], check=True)
