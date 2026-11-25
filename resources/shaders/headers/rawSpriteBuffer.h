/*
    This struct contains the info for every sprite after
    being processed by the object culling compute shader.
    The correct color and transforms are calculated
    in that compute shader and then written into the
    raw sprite buffer.
*
struct RawSprite {
    vec4 color;
    vec2 position;
    mat2 localTransform;
    uint _padding[2];
};

STORAGE_BUFFER(RAW_SPRITE_BUFFER_BINDING) RawSpriteBuffer {
    RawSprite u_rawSprites[CPP_ONLY(0)];
};
*/