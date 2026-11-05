/*
    Pass 1/3 of GPU culling: one invocation processes one sprite.
    A workgroup owns at most CULL_THREADS sorted sprites and a private output
    range. Each lane writes its visible triangles at a prefix-summed offset,
    preserving sprite order without atomics.
*/
#include <bgfx_compute.sh>
#include "bismuth_shared.sh"

BUFFER_RO(b_static, uvec4, 0);   // object infos + input sprites
BUFFER_RO(b_sorted, uvec4, 1);   // sorted sprites + work groups + layers
BUFFER_RO(b_frame,  uvec4, 2);   // color channels + group states + pulse entries
BUFFER_WR(b_culledTris, uint, 3);
BUFFER_WR(b_rawSprites, uvec4, 4);
BUFFER_WR(b_counters, uint, 5);

SHARED uint s_triangleCounts[CULL_THREADS];
SHARED uint s_triangleStarts[CULL_THREADS];

uint bkColorChannel(uint id)
{
    uvec4 v = b_frame[id / 4u];
    uint c = id % 4u;
    return c == 0u ? v.x : (c == 1u ? v.y : (c == 2u ? v.z : v.w));
}

#include "bismuth_uniforms.sh"

uvec2 bkPulseEntry(uint index)
{
    uvec4 v = b_frame[BK_PULSE_OFFSET + index / 2u];
    return (index % 2u) == 0u ? uvec2(v.x, v.y) : uvec2(v.z, v.w);
}

#include "bismuth_kernel.sh"
#include "bismuth_gamestate.sh"

NUM_THREADS(CULL_THREADS, 1, 1)
void main()
{
    uint workGroupIndex = uint(gl_WorkGroupID.x);
    if (workGroupIndex >= BK_WORK_GROUP_COUNT)
        return;

    uint tid = uint(gl_LocalInvocationIndex);
    BkGameState gs = bkLoadGameState();
    uvec4 workGroup = b_sorted[BK_WORK_GROUP_OFFSET + workGroupIndex];
    uint spriteCount = workGroup.y - workGroup.x;

    uint triangleStart = 0u;
    uint triangleCount = 0u;

    // WorkGroupScheduler guarantees this range is no larger than CULL_THREADS.
    if (tid < spriteCount) {
        uvec4 sorted = b_sorted[workGroup.x + tid];
        vec2 startPosition = vec2(uintBitsToFloat(sorted.x), uintBitsToFloat(sorted.y));
        uint groupIndex = sorted.z;
        uint spriteIndex = sorted.w;

        uint g = BK_GROUP_STATE_OFFSET + groupIndex * GROUP_STATE_STRIDE;
        BkGroupState state = bkDecodeGroupState(b_frame[g], b_frame[g + 1u], b_frame[g + 2u]);
        vec2 objectPosition = bkObjectPosition(state, startPosition);

        if (bkIsOnScreen(gs, objectPosition)) {
            uvec4 sprite = b_static[BK_INPUT_SPRITE_OFFSET + spriteIndex];
            uint o = sprite.x * OBJECT_INFO_STRIDE;
            BkObjectInfo object = bkDecodeObjectInfo(b_static[o], b_static[o + 1u], b_static[o + 2u]);
            BkSpriteResult result = bkProcessSprite(gs, state, object, sprite.y, objectPosition);

            if (result.visible) {
                triangleStart = sprite.z;
                triangleCount = sprite.w;
                b_rawSprites[spriteIndex * RAW_SPRITE_STRIDE + 0u] = uvec4(
                    floatBitsToUint(objectPosition.x),
                    floatBitsToUint(objectPosition.y),
                    bkPackColor(result.color),
                    0u
                );
                b_rawSprites[spriteIndex * RAW_SPRITE_STRIDE + 1u] = floatBitsToUint(result.localTransform);
            }
        }
    }

    s_triangleCounts[tid] = triangleCount;
    s_triangleStarts[tid] = triangleStart;
    barrier();

    // Exclusive prefix for this lane; lane zero also computes the group total.
    uint prefix = 0u;
    for (uint i = 0u; i < tid; i++)
        prefix += s_triangleCounts[i];

    if (tid == 0u) {
        uint total = 0u;
        for (uint i = 0u; i < uint(CULL_THREADS); i++)
            total += s_triangleCounts[i];
        b_counters[workGroupIndex * WORK_GROUP_COUNTER_STRIDE] = total;
    }

    for (uint t = 0u; t < triangleCount; t++)
        b_culledTris[workGroup.z + prefix + t] = s_triangleStarts[tid] + t;
}
