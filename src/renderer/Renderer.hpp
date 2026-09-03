#pragma once

#include <Geode/binding/GameObject.hpp>
#include <common.hpp>
#include "Geode/cocos/sprite_nodes/CCSpriteBatchNode.h"
#include "ObjectBatch.hpp"
#include "ObjectSorter.hpp"
#include "ObjectUtils.hpp"
#include "Shader.hpp"
#include <unordered_map>

#include "GroupManager.hpp"
#include "ObjectBatchNode.hpp"
#include "../../resources/shaders/shared.h"

using namespace geode;

class Renderer : public cocos2d::CCNode {
private:
    inline Renderer()
        : objectBatch(*this), groupManager(*this) {}
    ~Renderer() override;

    bool init(PlayLayer* layer);

    void generateBatchNodes();

    void terminate();

    void prepareShaderUniforms();

    void prepareColorChannelBuffer();

    void generateStaticRenderingBuffer(ObjectSorter& sorter);

    void draw() override;

    void updateDebugText();

protected:
    Shader* prepareDraw();

    void finishDraw();

    friend class GroupManager;
    friend class ObjectBatchNode;

public:
    void update(float dt) override;

    bool isColorChannelBlending(i32 channel);

    cocos2d::CCSpriteBatchNode* getSpriteBatchNodeWithLayerId(LayerKey id);

    inline cocos2d::CCTexture2D* getSpriteSheetTexture(SpriteSheet sheet) {
        if ((i32)sheet < 0 || (i32)sheet >= (i32)SpriteSheet::COUNT)
            return nullptr;
        return spriteSheets[(i32)sheet];
    }

    inline PlayLayer* getPlayLayer() {
        return layer;
    }

    inline usize getObjectSRBIndex(GameObject* object) {
        return objectSRBIndicies[object];
    }

    inline bool isEnabled() { return enabled; }

    inline void toggleDebugText() {
        debugTextEnabled = !debugTextEnabled;
    }

    inline bool canEnableDisableIngame() const { return ingameEnableDisable; }

    inline bool isPaused() {
        return layer->m_isPaused;
    }

    inline bool isUseIndexCulling() const { return useIndexCulling; }

    inline ObjectBatch& getObjectBatch() { return objectBatch; }

    bool useOptimizations();

    void setEnabled(bool enabled);

    inline GroupManager& getGroupManager() { return groupManager; }
    
    void reset();

    void drawLine(const glm::vec2& p1, const glm::vec2& p2, const glm::vec4& color);

public:
    static Ref<Renderer> create(PlayLayer* layer);

    /**
     * Returns the current renderer. If the game
     * is not in a PlayLayer. This returns nullptr.
     */
    static Ref<Renderer> get();

private:
    PlayLayer* layer;

    bool enabled = false;
    bool debugTextEnabled = false;
    bool ingameEnableDisable = false;

    bool useIndexCulling = false;

    u64 rendererStartTime = 0;

    usize spritesOnScreen = 0;

    std::unordered_map<GameObject*, usize> objectSRBIndicies;

    glm::vec2 cameraCenterPos;

    std::vector<ObjectBatchNode*> batchNodes;

    cocos2d::CCTexture2D* spriteSheets[(i32)SpriteSheet::COUNT] = { nullptr };

    Ref<cocos2d::CCLabelBMFont> debugText;
    Ref<cocos2d::CCLabelBMFont> debugTextOutline1;
    Ref<cocos2d::CCLabelBMFont> debugTextOutline2;

    GroupManager groupManager;

    ObjectBatch objectBatch;
    Shader* shader = nullptr;
    Shader* basicShader = nullptr;

    ColorChannelBuffer* colorChannelBuffer;
    Buffer* colorChannelBufferObject = nullptr;

    Buffer* srbBuffer = nullptr;

    RendererUniformBuffer uniforms;
    Buffer* uniformBuffer = nullptr;

    float gameTimer = 0.0;

    i64 groupStateCount;
    i64 renderedGameObjectCount;
    i64 renderTime;
};

void storeGLStates();

void restoreGLStates();
