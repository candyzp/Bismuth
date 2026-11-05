#pragma once

/*
    Builds a synthetic "level" in exactly the buffer formats the Geometry Dash
    side produces (see shaders/bismuth_shared.sh), exercising most features of
    the culling kernel: group transforms, rotation, audio scale, HSV, pulses,
    invisible blocks, glow/additive layers, culling at screen edges.
*/

#include <renderer/BatchData.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <random>

namespace test {

using namespace bismuth;

struct Scene {
    StaticBatchData staticData;
    SortedBatchData sorted;
    FrameBatchData frame;
    GameStateUniforms gameState;
    u32 width = 0, height = 0;
    u32 layerCount = 2; // 0 = normal, 1 = additive
};

inline u32 packRGBA(u8 r, u8 g, u8 b, u8 a) {
    return u32(r) | (u32(g) << 8) | (u32(b) << 16) | (u32(a) << 24);
}

inline u32 makeHSV(float hue, float sat, float val, bool satAdd, bool valAdd) {
    u32 h = u32(hue + 256.f);
    u32 s = u32((sat + (satAdd ? 1.f : 0.f)) * 127.5f);
    u32 v = u32((val + (valAdd ? 1.f : 0.f)) * 127.5f);
    u32 ret = (h & HSV_HUE_MASK) << HSV_HUE_BIT | (s & HSV_SAT_MASK) << HSV_SAT_BIT | (v & HSV_VAL_MASK) << HSV_VAL_BIT;
    if (satAdd) ret |= HSV_SAT_ADD;
    if (valAdd) ret |= HSV_VAL_ADD;
    return ret;
}

inline Scene makeScene(u32 width, u32 height, u32 objectCount, u32 seed, u32 spritesPerWorkGroup = CULL_THREADS) {
    Scene scene;
    scene.width = width;
    scene.height = height;

    std::mt19937 rng(seed);
    auto uniform = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto chance = [&](float p) { return uniform(0.f, 1.f) < p; };

    const u32 groupCount = 5;
    const u32 channelCount = 8;

    //// STATIC: object infos then input sprites ////
    struct Sprite { u32 object, type, firstTri, triCount, layer; glm::vec2 start; u32 group; };
    std::vector<Sprite> sprites;
    std::vector<UVec4>& st = scene.staticData.staticBuffer;

    for (u32 o = 0; o < objectCount; o++) {
        u32 flags = 0;
        float rotationSpeed = chance(0.2f) ? uniform(-180.f, 180.f) : 0.f;
        if (chance(0.15f)) flags |= OBJECT_FLAG_USES_AUDIO_SCALE;
        if (chance(0.3f) && (flags & OBJECT_FLAG_USES_AUDIO_SCALE)) flags |= OBJECT_FLAG_CUSTOM_AUDIO_SCALE;
        if (chance(0.1f)) flags |= OBJECT_FLAG_IS_ORB;
        if (chance(0.1f)) flags |= OBJECT_FLAG_IS_INVISIBLE_BLOCK;
        if (chance(0.1f)) flags |= OBJECT_FLAG_SPECIAL_GLOW_COLOR;
        if (chance(0.2f)) flags |= OBJECT_FLAG_HAS_BASE_HSV;
        if (chance(0.2f)) flags |= OBJECT_FLAG_HAS_DETAIL_HSV;
        if (chance(0.2f)) flags |= OBJECT_FLAG_IS_STATIC_OBJECT;

        u32 baseChannel = (u32)std::uniform_int_distribution<int>(1, channelCount)(rng);
        if (chance(0.05f)) baseChannel = COLOR_CHANNEL_LIGHTER;
        u32 detailChannel = (u32)std::uniform_int_distribution<int>(1, channelCount)(rng);

        st.push_back(UVec4(floatBits(rotationSpeed), flags, floatBits(0.4f), floatBits(1.1f)));
        st.push_back(UVec4(floatBits(uniform(0.f, 30.f)), floatBits(uniform(0.5f, 1.f)), baseChannel,
                           makeHSV(uniform(-180.f, 180.f), uniform(0.f, 2.f), uniform(0.f, 2.f), chance(0.5f), chance(0.5f))));
        st.push_back(UVec4(detailChannel,
                           makeHSV(uniform(-180.f, 180.f), uniform(-1.f, 1.f), uniform(-1.f, 1.f), true, true), 0, 0));

        // Some objects are placed off screen to exercise culling.
        glm::vec2 start(uniform(-150.f, width + 150.f), uniform(-150.f, height + 150.f));
        u32 group = (u32)std::uniform_int_distribution<int>(0, groupCount - 1)(rng);

        u32 spriteKinds = chance(0.5f) ? 2 : 1;
        for (u32 k = 0; k < spriteKinds; k++) {
            u32 type = k == 0 ? (chance(0.8f) ? SPRITE_TYPE_BASE : SPRITE_TYPE_BLACK) : (chance(0.5f) ? SPRITE_TYPE_DETAIL : SPRITE_TYPE_GLOW);
            u32 layer = type == SPRITE_TYPE_GLOW ? 1 : (chance(0.25f) ? 1 : 0);
            sprites.push_back({ o, type, 0, 2, layer, start, group });
        }
    }

    scene.staticData.inputSpriteOffset = (u32)st.size();
    scene.staticData.inputSpriteCount = (u32)sprites.size();

    // Meshes: a quad (2 triangles) per sprite, size and texture region vary.
    auto& vertices = scene.staticData.vertices;
    for (u32 s = 0; s < sprites.size(); s++) {
        auto& sprite = sprites[s];
        sprite.firstTri = (u32)vertices.size() / 3;
        float w = uniform(6.f, 40.f), h = uniform(6.f, 40.f);
        float u0 = uniform(0.f, 0.5f), v0 = uniform(0.f, 0.5f);
        float u1 = u0 + uniform(0.1f, 0.5f), v1 = v0 + uniform(0.1f, 0.5f);
        glm::vec2 p[4] = { {-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2} };
        glm::vec2 t[4] = { {u0, v1}, {u1, v1}, {u1, v0}, {u0, v0} };
        int idx[6] = { 0, 1, 2, 2, 3, 0 };
        for (int i : idx)
            vertices.push_back({ p[i].x, p[i].y, packTexCoord(t[i].x, t[i].y), s });
        st.push_back(UVec4(sprite.object, sprite.type, sprite.firstTri, sprite.triCount));
    }
    scene.staticData.triangleCount = (u32)vertices.size() / 3;

    //// SORTED BATCH ////
    // Layer-major, original order within a layer (like sorting by z order), then
    // work groups of `spritesPerWorkGroup` with private output ranges.
    std::vector<u32> order;
    for (u32 l = 0; l < scene.layerCount; l++)
        for (u32 s = 0; s < sprites.size(); s++)
            if (sprites[s].layer == l) order.push_back(s);

    auto& sb = scene.sorted.buffer;
    for (u32 s : order)
        sb.push_back(UVec4(floatBits(sprites[s].start.x), floatBits(sprites[s].start.y), sprites[s].group, s));

    std::vector<UVec4> workGroups;
    std::vector<UVec4> layers;
    u32 cursor = 0, outTri = 0;
    for (u32 l = 0; l < scene.layerCount; l++) {
        u32 firstWorkGroup = (u32)workGroups.size();
        u32 layerOut = outTri;
        while (cursor < order.size() && sprites[order[cursor]].layer == l) {
            u32 begin = cursor;
            u32 out = outTri;
            while (cursor < order.size() && sprites[order[cursor]].layer == l && cursor - begin < spritesPerWorkGroup) {
                outTri += sprites[order[cursor]].triCount;
                cursor++;
            }
            workGroups.push_back(UVec4(begin, cursor, out, l));
        }
        layers.push_back(UVec4(firstWorkGroup, (u32)workGroups.size() - firstWorkGroup, layerOut, 0));
    }

    scene.sorted.workGroupOffset = (u32)sb.size();
    scene.sorted.workGroupCount = (u32)workGroups.size();
    sb.insert(sb.end(), workGroups.begin(), workGroups.end());
    scene.sorted.layerOffset = (u32)sb.size();
    scene.sorted.layerCount = (u32)layers.size();
    sb.insert(sb.end(), layers.begin(), layers.end());

    //// FRAME ////
    auto& fb = scene.frame.buffer;
    fb.assign(COLOR_CHANNEL_UVEC4S, UVec4(0));
    auto setChannel = [&](u32 id, u32 color) { fb[id / 4][id % 4] = color; };
    for (u32 c = 1; c <= channelCount; c++)
        setChannel(c, packRGBA((u8)(rng() % 256), (u8)(rng() % 256), (u8)(rng() % 256), (u8)(128 + rng() % 128)));
    setChannel(COLOR_CHANNEL_BG, packRGBA(40, 60, 200, 255));
    setChannel(COLOR_CHANNEL_LBG, packRGBA(120, 140, 255, 255));
    setChannel(COLOR_CHANNEL_BLACK, packRGBA(0, 0, 0, 255));

    scene.frame.groupStateOffset = (u32)fb.size();
    for (u32 g = 0; g < groupCount; g++) {
        float angle = g == 2 ? 0.3f : 0.f;
        glm::mat2 positional = glm::mat2(1.f);
        if (angle != 0.f)
            positional = glm::mat2(std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle));
        glm::mat2 local = g == 3 ? glm::mat2(0.8f, 0.2f, -0.2f, 0.8f) : glm::mat2(1.f);
        glm::vec2 offset(g * 7.f, g == 1 ? 25.f : 0.f);
        float opacity = g == 4 ? 0.6f : 1.f;
        u32 pulseIndex = g == 1 ? 1 : 0; // group 1 is pulsing

        UVec4 a, b, c;
        std::memcpy(&a, &positional, sizeof(a));
        std::memcpy(&b, &local, sizeof(b));
        c = UVec4(floatBits(offset.x), floatBits(offset.y), floatBits(opacity), pulseIndex);
        fb.push_back(a); fb.push_back(b); fb.push_back(c);
    }

    scene.frame.pulseOffset = (u32)fb.size();
    // Pulse entries (flags, colorOrHSV): [0] end marker, then a list for group 1.
    std::vector<u32> pulses = {
        0, 0,
        PULSE_FLAG_IS_ACTIVE | PULSE_FLAG_IS_DETAIL_ONLY | 0x8000u, packRGBA(255, 0, 0, 255),
        PULSE_FLAG_IS_ACTIVE | PULSE_FLAG_IS_BASE_ONLY | 0x4000u, packRGBA(0, 255, 0, 255),
        PULSE_FLAG_IS_ACTIVE | PULSE_FLAG_IS_HSV | PULSE_FLAG_IS_DYNAMIC_HSV | (3u << PULSE_FLAG_CHANNEL_SHIFT) | 0x6000u,
            makeHSV(90.f, 1.f, 1.f, false, false),
        0, 0,
    };
    for (size_t i = 0; i < pulses.size(); i += 4)
        fb.push_back(UVec4(pulses[i], pulses[i + 1], i + 2 < pulses.size() ? pulses[i + 2] : 0, i + 3 < pulses.size() ? pulses[i + 3] : 0));

    //// GAME STATE ////
    auto& gs = scene.gameState;
    glm::mat4 mvp = glm::ortho(0.f, (float)width, 0.f, (float)height, -1024.f, 1024.f);
    std::memcpy(gs.mvp, &mvp, sizeof(gs.mvp));
    gs.timer = 1.25f;
    gs.audioScale = 0.8f;
    gs.cameraViewSize = glm::vec2(width, height);
    gs.cameraPosition = glm::vec2(0.f, 0.f);
    gs.cameraRight = glm::vec2(1.f, 0.f);
    gs.cameraUp = glm::vec2(0.f, 1.f);
    gs.cameraSize = glm::vec2(width, height);
    gs.winSize = glm::vec2(width, height);
    gs.screenRight = (float)width;
    gs.cameraUnzoomedX = width * 0.5f;
    gs.specialLightBGColor = glm::vec3(0.4f, 0.5f, 0.9f);
    gs.flags = 0;

    return scene;
}

} // namespace test
