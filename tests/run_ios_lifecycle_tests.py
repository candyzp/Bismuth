#!/usr/bin/env python3
"""Exercise production level handoff, rendered-frame scheduling, and chunking."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
renderer = (root / 'src/renderer/ios/RendererIOS.cpp').read_text()

def function(signature):
    start = renderer.index(signature)
    brace = renderer.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (renderer[end] == '{') - (renderer[end] == '}')
        end += 1
    return renderer[start:end]

def source(path):
    return '\n'.join(line for line in (root / path).read_text().splitlines()
                     if not line.startswith(('#include', '#pragma once')))

fixture = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using usize=std::size_t;
namespace cocos2d {
struct CCSprite {};
struct CCSpriteBatchNode {};
struct CCNode { template<class T> void addChild(T, int) {} };
struct CCDirector { inline static std::function<void()> frame; virtual void drawScene(){frame();} };
}
struct GameObject : cocos2d::CCSprite { float x=0; float getPositionX(){return x;} };
struct PlayLayer {
    cocos2d::CCNode objectLayer;
    cocos2d::CCNode* m_objectLayer=&objectLayer;
    virtual ~PlayLayer()=default;
    virtual void resetLevel() {}
    virtual void setupHasCompleted() { resetLevel(); }
    virtual void onExit() {} virtual void onEnterTransitionDidFinish() {}
};
namespace geode {
template<class T> struct Ref {
    T* p=nullptr;
    Ref(T* value=nullptr):p(value){}
    T* operator->() const{return p;}
    operator T*() const{return p;}
};
namespace prelude { namespace log { template<class... T> void error(T&&...) {} } }
}
using namespace geode;
using namespace geode::prelude;
struct Mod {
    inline static bool enabled=true, debug=false;
    static Mod* get(){static Mod m; return &m;}
    template<class T> T getSettingValue(const char* name){return std::string(name)=="enabled" ? enabled : debug;}
};
struct ColorChannelBuffer {};
struct Shader { static void destroy(Shader*){} };
struct ResolvedStateLayer {
    struct ShadowCandidate { GameObject* object=nullptr; cocos2d::CCSprite* sprite=nullptr; usize objectStateIndex=0, spriteStateIndex=0; };
    inline static ResolvedStateLayer* current=nullptr;
    int updates=0, finishes=0, seeds=0, syncs=0;
    static ResolvedStateLayer* getCurrent(){return current;}
    void setCurrent(bool active){if(active)current=this;else if(current==this)current=nullptr;}
    void resync(){++syncs;} void reseedActiveFromStock(){++seeds;}
    void update(bool){++updates;} void finishEventFrame(){++finishes;}
};
struct AssistShadowBatch {};
struct StandaloneAssistBatch { int begins=0; void beginFrame(){++begins;} };
struct AtlasInterleaveRegistry { inline static int begins=0; static void beginFrame(){++begins;} };
struct Renderer {
    PlayLayer* layer=nullptr; bool enabled=true; float gameTimer=0; int debugCalls=0, restores=0;
    static Ref<Renderer> get(); static Ref<Renderer> create(PlayLayer*);
    static Ref<Renderer> forPlayLayer(PlayLayer*);
    PlayLayer* getPlayLayer(){return layer;} bool isEnabled(){return enabled;}
    void update(float); void beginGPUFrame(); void prepareGPUFrame(); void finishGPUFrame();
    void suspendGPU(); void resumeGPU(); void reset() {}
    void setEnabled(bool value){enabled=value;if(!value)++restores;}
    void updateDebugText(){++debugCalls;}
};
#define $modify(Name, Base) Name : public Base
'''
state = renderer[renderer.index('struct IOSRendererState {'):renderer.index('static bool isDescendantOf(')]
# This section contains the production renderer state and lookup (no GL calls).
methods = '\n'.join(function(sig) for sig in [
    'void Renderer::update(float dt)', 'void Renderer::beginGPUFrame()',
    'void Renderer::prepareGPUFrame()', 'void Renderer::finishGPUFrame()',
    'Ref<Renderer> Renderer::forPlayLayer(', 'void Renderer::suspendGPU()',
    'void Renderer::resumeGPU()',
])
setup = source('src/renderer/hooks.cpp')
setup = setup[setup.index('static bool newPlayLayer'):setup.index('    void updateVisibility')]+ '};'
chunks = renderer[renderer.index('struct StandaloneObjectDesc'):renderer.index('struct IOSRendererState')]
chunks += '\nconstexpr usize MAX_STANDALONE_BUFFER_SPRITES=16383;\n'
chunks += function('static std::vector<StandaloneChunkDesc> buildStandaloneChunks(')
implementation = r'''
static std::vector<std::unique_ptr<Renderer>> allocated;
Ref<Renderer> Renderer::get(){return currentRenderer;}
Ref<Renderer> Renderer::create(PlayLayer* layer){
    auto renderer=std::make_unique<Renderer>(); auto r=renderer.get(); r->layer=layer;
    auto state=std::make_unique<IOSRendererState>();
    state->resolvedState=std::make_unique<ResolvedStateLayer>(); state->resolvedState->setCurrent(true);
    g_iosStates[r]=std::move(state); currentRenderer=r;
    allocated.push_back(std::move(renderer)); return r;
}
'''
cases = r'''
int main(){
    RendererPlayLayer first, second;
    static_cast<PlayLayer&>(first).setupHasCompleted(); auto old=Renderer::forPlayLayer(&first);
    assert(old && currentRenderer==old && old->isEnabled());
    static_cast<PlayLayer&>(second).setupHasCompleted(); auto next=Renderer::forPlayLayer(&second);
    assert(next && next!=old && currentRenderer==next && !old->isEnabled());
    old->suspendGPU(); // Late exit of the old scene must not clear the new owner.
    assert(currentRenderer==next && old->restores==1);
    auto state=iosState(next);
    assert(ResolvedStateLayer::getCurrent()==state->resolvedState.get());
    next->suspendGPU();
    assert(currentRenderer==nullptr && !next->isEnabled());
    static_cast<PlayLayer&>(second).resetLevel();
    assert(currentRenderer==next && next->isEnabled());
    assert(ResolvedStateLayer::getCurrent()==state->resolvedState.get());
    next->setEnabled(false);
    next->suspendGPU();
    static_cast<PlayLayer&>(second).resetLevel();
    assert(!next->isEnabled());
    next->setEnabled(true);
    old->resumeGPU(); assert(currentRenderer==old && old->isEnabled() && !next->isEnabled());
    next->resumeGPU(); assert(currentRenderer==next && next->isEnabled());
    next->setEnabled(false); next->suspendGPU(); next->suspendGPU(); next->resumeGPU();
    assert(!next->isEnabled()); // A deliberate disable survives suspend/resume.
    next->setEnabled(true);
    static_cast<PlayLayer&>(second).setupHasCompleted();
    assert(Renderer::forPlayLayer(&second)==next && allocated.size()==2);
    RendererExitGuardPlayLayer returning;
    next->suspendGPU();
    auto returningRenderer=Renderer::create(&returning);
    static_cast<PlayLayer&>(returning).onExit();
    assert(currentRenderer==nullptr && !returningRenderer->isEnabled());
    static_cast<PlayLayer&>(returning).onEnterTransitionDidFinish();
    assert(currentRenderer==returningRenderer && returningRenderer->isEnabled());
    next->resumeGPU();

    BismuthFrameVisualSync director;
    Mod::debug=true;
    int frames=0;
    cocos2d::CCDirector::frame=[&]{
        const int before=state->resolvedState->updates;
        for(int i=0;i<8;++i) next->update(0.004f);
        assert(state->resolvedState->updates==before);
        next->prepareGPUFrame(); next->prepareGPUFrame();
        assert(state->resolvedState->updates==before+1);
        state->batchTransformSkipsCurrentFrame=42;
        ++frames;
    };
    for(int i=0;i<120;++i) static_cast<cocos2d::CCDirector&>(director).drawScene();
    assert(frames==120 && state->resolvedState->updates==120 && state->resolvedState->finishes==120);
    assert(state->batchTransformSkipsLastFrame==42 && next->debugCalls<15);
    const int before=state->resolvedState->updates;
    cocos2d::CCDirector::frame=[]{}; static_cast<cocos2d::CCDirector&>(director).drawScene();
    assert(state->resolvedState->updates==before+1 && state->batchTransformSkipsLastFrame==0);

    // More than two u16 buffers, supplied in reverse spatial order.
    std::vector<GameObject> objects(33000);
    std::vector<StandaloneObjectDesc> descriptions;
    for(usize i=objects.size();i-->0;){
        objects[i].x=static_cast<float>(i);
        descriptions.push_back({&objects[i], {{&objects[i], &objects[i], i, i}}});
    }
    auto buffers=buildStandaloneChunks(descriptions);
    assert(buffers.size()==3 && buffers[0].roots.size()==16383 && buffers[2].roots.size()==234);
    usize index=0;
    for(auto& buffer:buffers) for(auto root:buffer.roots) assert(root==&objects[index++]);
    currentRenderer=nullptr; g_iosStates.clear(); allocated.clear();
    std::cout<<"PASS: overlapping level setup/exit, resume/disable, 960 simulation updates in 120 renders with 120 captures, empty render, spatial u16 chunking\n";
}
'''
code='\n'.join(['#define GEODE_IS_IOS', fixture, state, methods, implementation, setup, chunks,
    source('src/renderer/ios/PlayLayerExitGuard.cpp'), source('src/renderer/ios/FrameVisualSync.cpp'), cases])
with tempfile.TemporaryDirectory(prefix='bismuth-lifecycle-tests-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(code)
    subprocess.run(['g++','-std=c++23','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
