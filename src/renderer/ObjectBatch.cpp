#include "ObjectBatch.hpp"
#include "Renderer.hpp"
#include "SpriteMeshDictionary.hpp"
#include "common.hpp"
#include "glm/fwd.hpp"
#include "math/ConvexList.hpp"
#include "math/ConvexPolygon.hpp"
#include "../profiler.hpp"

using namespace geode::prelude;

SpriteVertexTransforms ObjectBatch::getSpriteVertexTransform(
    cocos2d::CCSprite* sprite,
    const cocos2d::CCAffineTransform& transform,
    SpriteSheet spriteSheet
) {
    CCRect crop = sprite->getTextureRect();
    
    CCTexture2D* texture = renderer.getSpriteSheetTexture(spriteSheet);
    if (texture == nullptr) return {};

    glm::vec2 posBottomLeft  = ccPointToGLM(CCPointApplyAffineTransform(sprite->getOffsetPosition(), transform));
    glm::vec2 posRightVector = glm::vec2(transform.a, transform.b) * crop.size.width;
    glm::vec2 posUpVector    = glm::vec2(transform.c, transform.d) * crop.size.height;

    glm::vec2 texBottomLeft;
    glm::vec2 texRightVector;
    glm::vec2 texUpVector;

    if (!sprite->isTextureRectRotated()) {
        texBottomLeft  = { crop.origin.x, crop.origin.y + crop.size.height };
        texRightVector = { crop.size.width, 0 };
        texUpVector    = { 0, -crop.size.height };
    } else {
        texBottomLeft  = { crop.origin.x, crop.origin.y };
        texRightVector = { 0, crop.size.width };
        texUpVector    = { crop.size.height, 0 };
    }
    
    if (sprite->isFlipX()) {
        posBottomLeft += posRightVector;
        posRightVector = -posRightVector;
    }
    if (sprite->isFlipY()) {
        posBottomLeft += posUpVector;
        posUpVector = -posUpVector;
    }

    float contentScaleFactor = CCDirector::get()->getContentScaleFactor();
    glm::vec2 texCoordFactor = glm::vec2(
        contentScaleFactor / texture->getPixelsWide(),
        contentScaleFactor / texture->getPixelsHigh()
    );

    texBottomLeft  *= texCoordFactor;
    texRightVector *= texCoordFactor;
    texUpVector    *= texCoordFactor;

    return {
        posBottomLeft, posRightVector, posUpVector,
        texBottomLeft, texRightVector, texUpVector
    };
}

bool ObjectBatch::prepareSpriteMeshWrite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    const cocos2d::CCAffineTransform& transform
) {
    SpriteSheet spriteSheet = ObjectUtils::getSpritesheetOfObject(object, type);
    if (spriteSheetFilter != (SpriteSheet)-1 && spriteSheet != spriteSheetFilter)
        return false;

    // Without a texture the sprite would be written as an empty quad which
    // would still take up buffer space and a work group.
    if (!renderer.getSpriteSheetTexture(spriteSheet))
        return false;

    currentSpriteVertexTransforms    = getSpriteVertexTransform(sprite, transform, spriteSheet);
    currentSpriteObjectStartPosition = ccPointToGLM(object->m_startPosition);

    currentSpriteObjectIndex = renderer.getObjectIndex(object);
    currentSpriteVertexIndex = verticies.size();

    return true;
}

void ObjectBatch::writeSpriteVertex(glm::vec2 pos) {
    isize index = verticies.size();
    verticies.resize(index + 1);
    ObjectVertex& vertex = verticies[index];

    auto& transforms = currentSpriteVertexTransforms;

    vertex.positionOffset = transforms.positionRight * pos.x +
                            transforms.positionUp    * pos.y +
                            transforms.positionBottomLeft - currentSpriteObjectStartPosition;
    
    vertex.texCoord = transforms.texCoordRight * pos.x +
                      transforms.texCoordUp    * pos.y +
                      transforms.texCoordBottomLeft;

    vertex.spriteIndex = currentSpriteIndex;
}

/*
void ObjectBatch::writeSpriteIndex(u32 index) {
    indicies.push_back(currentSpriteVertexIndex + index);
}
*/

void ObjectBatch::writeSpriteMeshFromConvexList(const ConvexList& list) {
    u32 vertexIndex = 0;
    list.triangulate([&](const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3) {
        writeSpriteVertex(p1);
        writeSpriteVertex(p2);
        writeSpriteVertex(p3);
        // writeSpriteIndex(vertexIndex + 0);
        // writeSpriteIndex(vertexIndex + 1);
        // writeSpriteIndex(vertexIndex + 2);
        vertexIndex += 3;
    });
}

static bool doesObjectHaveCustomVanillaMesh(GameObject* object) {
    int id = object->m_objectID;

    return id >= 1769 && id <= 1810 && id != 1798;
}

void ObjectBatch::addSprite(
    GameObject* object,
    cocos2d::CCSprite* sprite,
    SpriteType type,
    const cocos2d::CCAffineTransform& transform
) {
    if (object->m_hasNoGlow && type == SpriteType::GLOW)
        return;

    if (!prepareSpriteMeshWrite(object, sprite, type, transform))
        return;

    spriteCount++;

    usize trianglesStart = verticies.size() / 3;

    currentSpriteIndex = inputSprites.size();

    if (doesObjectHaveCustomVanillaMesh(object)) {
        addSpriteWithCustomVanillaMesh(sprite);
    } else {
        ConvexList* spriteMesh = SpriteMeshDictionary::getSpriteMeshForSprite(sprite);

        if (spriteMesh) {
            writeSpriteMeshFromConvexList(*spriteMesh);
        } else {
            writeSpriteVertex({ 0, 0 });
            writeSpriteVertex({ 1, 0 });
            writeSpriteVertex({ 1, 1 });
            writeSpriteVertex({ 1, 1 });
            writeSpriteVertex({ 0, 1 });
            writeSpriteVertex({ 0, 0 });

            // writeSpriteIndex(QUAD_BL);
            // writeSpriteIndex(QUAD_TL);
            // writeSpriteIndex(QUAD_TR);
            // writeSpriteIndex(QUAD_BL);
            // writeSpriteIndex(QUAD_TR);
            // writeSpriteIndex(QUAD_BR);
        }
    }

    usize trianglesCount = verticies.size() / 3 - trianglesStart;

    inputSprites.push_back({
        .objectIndex = (u32)currentSpriteObjectIndex,
        .spriteType = (u32)type,
        .indexStart = (u32)trianglesStart,
        .indexCount = (u32)trianglesCount
    });

    ZLayer zLayer = object->getObjectZLayer();
    SpriteSheet spriteSheet = ObjectUtils::getSpritesheetOfObject(object, type);

    usedLayerKeys.insert({zLayer, spriteSheet, false});
    usedLayerKeys.insert({zLayer, spriteSheet, true});

    usedLayerKeys.insert({(ZLayer)((int)zLayer + 1), spriteSheet, false});
    usedLayerKeys.insert({(ZLayer)((int)zLayer + 1), spriteSheet, true});

    workGroupScheduler.addSprite(object, sprite, type, currentSpriteIndex, trianglesCount);
}

void ObjectBatch::addSpriteWithCustomVanillaMesh(cocos2d::CCSprite* sprite) {
    auto quad = sprite->getQuad();

    CCPoint offset = sprite->getOffsetPosition();
    CCPoint size   = sprite->getTextureRect().size;

    CCPoint bl = (ccp(quad.bl.vertices.x, quad.bl.vertices.y) - offset) / size;
    CCPoint br = (ccp(quad.br.vertices.x, quad.br.vertices.y) - offset) / size;
    CCPoint tl = (ccp(quad.tl.vertices.x, quad.tl.vertices.y) - offset) / size;
    CCPoint tr = (ccp(quad.tr.vertices.x, quad.tr.vertices.y) - offset) / size;

    writeSpriteVertex(ccPointToGLM(bl));
    writeSpriteVertex(ccPointToGLM(br));
    writeSpriteVertex(ccPointToGLM(tr));
    writeSpriteVertex(ccPointToGLM(tr));
    writeSpriteVertex(ccPointToGLM(tl));
    writeSpriteVertex(ccPointToGLM(bl));
}

void ObjectBatch::writeGameObject(GameObject* object) {
    float originalScaleX = object->getScaleX();
    float originalScaleY = object->getScaleY();

    if (object->m_usesAudioScale) {
        object->setScaleX(object->m_scaleX);
        object->setScaleY(object->m_scaleY);
    }

    ObjectUtils::unpackObjectIntoSprites(object, [&](const UnpackedSprite& sprite) {
        addSprite(sprite.parentObject, sprite.sprite, sprite.type, sprite.transform);
    });

    object->setScaleX(originalScaleX);
    object->setScaleY(originalScaleY);
}

void ObjectBatch::finishWriting() {
    assert(vertexBuffer == nullptr);

    vertexCount = verticies.size();
    // indexCount  = indicies.size();

    vertexBuffer      = Buffer::createStaticDraw("VertexBuffer", verticies.data(), vertexCount * sizeof(ObjectVertex));
    inputSpriteBuffer = Buffer::createStaticDraw("InputSpriteBuffer", inputSprites.data(), inputSprites.size() * sizeof(InputSprite));

    rawSpriteBuffer   = RawSprite::Buffer::create(getSpriteCount(), GL_DYNAMIC_DRAW, false);
    culledIndexBuffer = Buffer::createGPUExclusive("CulledTriIndexBuffer", vertexCount / 3 * sizeof(u32));

    if (!vertexBuffer || !inputSpriteBuffer || !rawSpriteBuffer || !culledIndexBuffer)
        log::error("Failed to create one or more object batch buffers");

    preCulledSpritesBuffer = PreCulledSprite::Buffer::create(getSpriteCount(), GL_STATIC_DRAW);

    if (preCulledSpritesBuffer) {
        workGroupScheduler.writePreCulledSprites(*preCulledSpritesBuffer);
        preCulledSpritesBuffer->flushAllAndUnmap();
    } else
        log::error("Failed to create the pre culled sprite buffer");

    workGroupScheduler.finalizeSpriteEntries();

    verticies.clear();
    // indicies.clear();
    inputSprites.clear();
}

void ObjectBatch::draw(const LayerKey& layer) {
    if (!vertexBuffer || !culledIndexBuffer || !rawSpriteBuffer || !sortedBatch)
        return;

    auto layerIt = sortedBatch->workGroupRangePerLayerKey.find(layer);

    if (layerIt == sortedBatch->workGroupRangePerLayerKey.end() || layerIt->second.count == 0)
        return;

    auto spriteSheet = renderer.getSpriteSheetTexture(layer.spriteSheet);
    if (!spriteSheet)
        return;

    const auto& range = layerIt->second;

    auto shader = renderer.getDrawingShader();
    if (!shader)
        return;

    /*
        This used to ask GL for the current program every layer, which is a
        synchronous driver query. glUseProgram with the program that is
        already bound is free by comparison, and the storage buffer bindings
        only have to be set up again when the batch changes or the culling
        pass replaced the program.
    */
    shader->use();

    if (!drawStateIsBound || drawStateBoundBatch != sortedBatch) {
        rawSpriteBuffer->bind();
        sortedBatch->drawCommandBuffer->bindAs(GL_DRAW_INDIRECT_BUFFER);
        vertexBuffer->bindAsStorageBuffer(VERTEX_BUFFER_BINDING);
        culledIndexBuffer->bindAsStorageBuffer(CULLED_TRI_INDEX_BUFFER);

        drawStateIsBound = true;
        drawStateBoundBatch = sortedBatch;
    }

    ccGLBindTexture2D(spriteSheet->getName());

    if (layer.blending || layer.spriteSheet == SpriteSheet::GLOW)
        ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else
        ccGLBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glMultiDrawArraysIndirect(
        GL_TRIANGLES,
        (void*)(range.offset * sizeof(DrawCommand)),
        range.count,
        sizeof(DrawCommand)
    );
}

void ObjectBatch::predraw() {
    PROFILER_SCOPE("ObjectBatch::predraw");
    cullingComputeShaderTimer.start();

    const auto& blendStates = renderer.getColorChannelBlendStates();

    if (blendStatesSeen && blendStates == lastSeenBlendStates)
        stableBlendStateFrames++;
    else {
        lastSeenBlendStates = blendStates;
        blendStatesSeen = true;
        stableBlendStateFrames = 0;
    }

    bool needsNewBatch = !sortedBatch || !haveBatchForRequestedStates || !(blendStates == requestedBlendStates);

    if (needsNewBatch) {
        // Without a batch at all there is nothing to draw, so request one
        // right away. Otherwise wait until the blend state is stable so a
        // state that only lasts a frame or two never rebuilds the whole level.
        bool mayRegenerate = !sortedBatch ||
            stableBlendStateFrames >= REGENERATE_AFTER_STABLE_FRAMES;

        if (mayRegenerate) {
            ScopedFrameTimer timer(renderer.getFrameTiming().sortedBatch);

            // This uses a different thread to avoid a lag spike
            auto newSortedBatch = workGroupScheduler.fetchSortedBatchForBlendStates(blendStates, sortedBatch);

            if (newSortedBatch) {
                if (newSortedBatch != sortedBatch) {
                    renderer.getFrameTiming().countSortedBatchGeneration();

                    // New buffers are bound under the new batch on the next draw.
                    drawStateIsBound = false;
                    drawStateBoundBatch = nullptr;
                }

                sortedBatch = newSortedBatch;
                requestedBlendStates = blendStates;
                haveBatchForRequestedStates = true;
            } else if (!sortedBatch) {
                workGroupScheduler.waitForJobs();
                sortedBatch = workGroupScheduler.fetchSortedBatchForBlendStates(blendStates, nullptr);

                if (!sortedBatch)
                    return;

                requestedBlendStates = blendStates;
                haveBatchForRequestedStates = true;
            }
        }
    }

    if (!sortedBatch)
        return;

    Shader* computeShader = renderer.getCullingComputeShader();
    if (!computeShader)
        return;

    computeShader->use();

    prepareCullingComputeShader();
    renderer.bindCullingComputeShaderBuffers();
    sortedBatch->bindBuffers();

    glDispatchCompute(sortedBatch->workGroupCount, 1, 1);

    /*
        The compute pass writes the culled indices, the raw sprite data and
        the indirect draw commands, all of which the draw pass reads next.
        The spec requires a barrier between the two, otherwise a driver is
        free to start drawing before the writes are visible.
    */
    glMemoryBarrier(
        GL_SHADER_STORAGE_BARRIER_BIT |
        GL_COMMAND_BARRIER_BIT |
        GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT
    );

    // The compute program is current now, so the drawing program has to be
    // bound again before the next layer draws.
    drawStateIsBound = false;

    cullingComputeShaderTimer.stop();
}

void ObjectBatch::prepareCullingComputeShader() {
    if (!inputSpriteBuffer || !rawSpriteBuffer || !preCulledSpritesBuffer)
        return;

    inputSpriteBuffer->bindAsStorageBuffer(INPUT_SPRITE_BUFFER_BINDING);
    preCulledSpritesBuffer->bind();
    rawSpriteBuffer->bind();
}