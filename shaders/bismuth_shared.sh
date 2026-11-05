/*
    Constants shared between the C++ code and every shader.

    This file must only contain #define directives so that it can be included
    from C++, GLSL, HLSL (SPIR-V / Metal through shaderc) alike.

    GPU buffers only use `uint` and `uvec4` elements: those have identical
    layouts on every backend (std430, HLSL structured buffers, Metal).
*/

#ifndef BISMUTH_SHARED_SH
#define BISMUTH_SHARED_SH

//// SPRITE TYPES ////
#define SPRITE_TYPE_BASE   0u
#define SPRITE_TYPE_DETAIL 1u
#define SPRITE_TYPE_BLACK  2u
#define SPRITE_TYPE_GLOW   3u

//// COLOR CHANNELS ////
#define COLOR_CHANNEL_COUNT   1101u

#define COLOR_CHANNEL_BG      1000u
#define COLOR_CHANNEL_G1      1001u
#define COLOR_CHANNEL_LINE    1002u
#define COLOR_CHANNEL_3DL     1003u
#define COLOR_CHANNEL_OBJ     1004u
#define COLOR_CHANNEL_P1      1005u
#define COLOR_CHANNEL_P2      1006u
#define COLOR_CHANNEL_LBG     1007u
#define COLOR_CHANNEL_G2      1009u
#define COLOR_CHANNEL_BLACK   1010u
#define COLOR_CHANNEL_WHITE   1011u
#define COLOR_CHANNEL_LIGHTER 1012u
#define COLOR_CHANNEL_MG      1013u
#define COLOR_CHANNEL_MG2     1014u

#define A_COLOR_CHANNEL_IS_SPRITE_DETAIL 0x1000u

//// OBJECT FLAGS ////
#define OBJECT_FLAG_USES_AUDIO_SCALE   (1u << 0)
#define OBJECT_FLAG_CUSTOM_AUDIO_SCALE (1u << 1)
#define OBJECT_FLAG_IS_ORB             (1u << 2)
#define OBJECT_FLAG_IS_INVISIBLE_BLOCK (1u << 3)
#define OBJECT_FLAG_SPECIAL_GLOW_COLOR (1u << 4)
#define OBJECT_FLAG_IS_GLOW_COLOR_LBG  (1u << 5)
#define OBJECT_FLAG_HAS_BASE_HSV       (1u << 6)
#define OBJECT_FLAG_HAS_DETAIL_HSV     (1u << 7)
#define OBJECT_FLAG_IS_STATIC_OBJECT   (1u << 8)

//// GAME STATE FLAGS ////
#define GAME_STATE_IS_PLAYER_DEAD 1u

//// HSV ENCODING ////
#define HSV_HUE_BIT  16u
#define HSV_HUE_MASK 0x1ffu
#define HSV_SAT_BIT  8u
#define HSV_SAT_MASK 0xffu
#define HSV_VAL_BIT  0u
#define HSV_VAL_MASK 0xffu
#define HSV_SAT_ADD  0x20000000u
#define HSV_VAL_ADD  0x10000000u

//// PULSE ENTRIES ////
#define PULSE_FLAG_IS_BASE_ONLY   (1u << 26)
#define PULSE_FLAG_IS_DETAIL_ONLY (1u << 27)
#define PULSE_FLAG_IS_HSV         (1u << 28)
#define PULSE_FLAG_IS_DYNAMIC_HSV (1u << 29)
#define PULSE_FLAG_IS_ACTIVE      (1u << 30)
#define PULSE_FLAG_VALUE_MASK     0xffffu
#define PULSE_FLAG_CHANNEL_SHIFT  16u
#define PULSE_FLAG_CHANNEL_MASK   0x3ffu
// Upper bound for pulse entries applied to one sprite (guards against bad data).
#define PULSE_MAX_ENTRIES_PER_SPRITE 64u

//// CULLING ////
// Distance in world units around the camera rectangle in which sprites are kept.
#define CULL_SCREEN_THRESHOLD 100.0f

//// BUFFER LAYOUTS (in uvec4 elements unless noted) ////

// Static object buffer (created once per level):
//   [STATIC_OBJECTS_OFFSET + i*3 + 0] = (rotationSpeed bits, flags, audioScaleMin bits, audioScaleMax bits)
//   [STATIC_OBJECTS_OFFSET + i*3 + 1] = (fadeMargin bits, opacity bits, baseColorChannel, baseHSV)
//   [STATIC_OBJECTS_OFFSET + i*3 + 2] = (detailColorChannel, detailHSV, 0, 0)
//   [inputSpriteOffset + s]           = (objectIndex, spriteType, firstTriangle, triangleCount)
#define OBJECT_INFO_STRIDE 3u

// Sorted batch buffer (one per color blending state permutation):
//   [s]                     = (startPosition.x bits, startPosition.y bits, groupCombinationIndex, inputSpriteIndex)
//   [workGroupOffset + w]   = (firstSortedSprite, endSortedSprite, outputTriangleOffset, layerIndex)
//   [layerOffset + l]       = (firstWorkGroup, workGroupCount, outputTriangleOffset, 0)

// Frame buffer (rewritten every frame):
//   [0 .. COLOR_CHANNEL_UVEC4S)   = color channels, RGBA8 packed, 4 per uvec4
//   [groupStateOffset + g*3 + 0]  = positionalTransform (column major mat2) as float bits
//   [groupStateOffset + g*3 + 1]  = localTransform (column major mat2) as float bits
//   [groupStateOffset + g*3 + 2]  = (offset.x bits, offset.y bits, opacity bits, pulseIndex)
//   [pulseOffset + p/2]           = pulse entries, (flags, colorOrHSV) pairs, 2 per uvec4
#define COLOR_CHANNEL_UVEC4S 276u
#define GROUP_STATE_STRIDE   3u

// Raw sprite buffer (written by culling): 2 uvec4 per sprite
//   [s*2 + 0] = (position.x bits, position.y bits, color RGBA8, 0)
//   [s*2 + 1] = localTransform (column major mat2) as float bits
#define RAW_SPRITE_STRIDE 2u

// Work group counter buffer (uint): [w*2 + 0] = visible triangles, [w*2 + 1] = compacted offset
#define WORK_GROUP_COUNTER_STRIDE 2u

// Object vertex buffer (uvec4 per vertex):
//   (positionOffset.x bits, positionOffset.y bits, texCoord packed unorm16x2, spriteIndex)

// Compute shader thread counts. One GPU cull lane handles one sprite.
#define CULL_THREADS 64
#define SCAN_THREADS 64
#define COPY_THREADS 64

//// UNIFORMS ////
// u_gameState[0] = (timer, audioScale, cameraViewSize.x, cameraViewSize.y)
// u_gameState[1] = (cameraPosition.x, cameraPosition.y, cameraRight.x, cameraRight.y)
// u_gameState[2] = (cameraUp.x, cameraUp.y, cameraSize.x, cameraSize.y)
// u_gameState[3] = (winSize.x, winSize.y, screenRight, cameraUnzoomedX)
// u_gameState[4] = (specialLightBGColor.rgb, gameStateFlags)
// u_gameState[5] = (groupStateOffset, pulseOffset, inputSpriteOffset, workGroupOffset)
// u_gameState[6] = (layerOffset, workGroupCount, layerCount, 0)
#define GAME_STATE_VEC4S 7

#endif // BISMUTH_SHARED_SH
