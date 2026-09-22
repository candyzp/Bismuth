precision highp float;

// Static batch geometry is authored in GameObject-local space. Geometry Dash
// remains responsible for resolving the root object's final state; this shader
// only performs the repetitive per-vertex math in parallel.
attribute vec2 a_localPosition;
attribute vec2 a_texCoord;
attribute float a_objectStateIndex;
attribute float a_spriteStateIndex;

uniform highp sampler2D u_objectStateTexture;
uniform highp sampler2D u_spriteStateTexture;
uniform vec2 u_objectStateTextureSize;
uniform vec2 u_spriteStateTextureSize;
uniform mat4 u_mvp;

varying vec2 t_texCoord;
varying vec4 t_color;

vec4 fetchData(highp sampler2D textureSampler, vec2 textureSize, float index) {
    float x = mod(index, textureSize.x);
    float y = floor(index / textureSize.x);
    return texture2D(textureSampler, (vec2(x, y) + vec2(0.5)) / textureSize);
}

void main() {
    t_texCoord = a_texCoord;

    float spriteIndex = floor(a_spriteStateIndex + 0.5);
    float spriteBase = spriteIndex * 4.0;
    vec4 resolvedColor = fetchData(u_spriteStateTexture, u_spriteStateTextureSize, spriteBase + 0.0);
    vec4 spriteMeta = fetchData(u_spriteStateTexture, u_spriteStateTextureSize, spriteBase + 1.0);

    float visibleBit = mod(floor(spriteMeta.x), 2.0);
    if (visibleBit < 0.5) {
        gl_Position = vec4(4.0, 4.0, 4.0, 1.0);
        t_color = vec4(0.0);
        return;
    }

    // Bit 4 means Cocos supplied its authoritative sprite->batch matrix.
    // In this branch Cocos is also the sole visibility authority. Do not let
    // Bismuth's object lifecycle hide a sprite that Cocos has already placed
    // into the live atlas for this frame.
    float hasBatchTransform = mod(floor(spriteMeta.x / 16.0), 2.0);
    if (hasBatchTransform > 0.5) {
        vec4 s0 = fetchData(u_spriteStateTexture, u_spriteStateTextureSize, spriteBase + 2.0);
        vec4 s1 = fetchData(u_spriteStateTexture, u_spriteStateTextureSize, spriteBase + 3.0);
        if (s1.w < 0.5) {
            gl_Position = vec4(4.0, 4.0, 4.0, 1.0);
            t_color = vec4(0.0);
            return;
        }
        vec2 batchPosition = vec2(
            a_localPosition.x * s0.x + a_localPosition.y * s0.z + s1.x,
            a_localPosition.x * s0.y + a_localPosition.y * s0.w + s1.y
        );
        gl_Position = u_mvp * vec4(batchPosition, s1.z, 1.0);
    } else {
        float objectIndex = floor(a_objectStateIndex + 0.5);
        float objectBase = objectIndex * 2.0;
        vec4 o0 = fetchData(u_objectStateTexture, u_objectStateTextureSize, objectBase + 0.0);
        vec4 o1 = fetchData(u_objectStateTexture, u_objectStateTextureSize, objectBase + 1.0);

        if (o1.w < 0.5) {
            gl_Position = vec4(4.0, 4.0, 4.0, 1.0);
            t_color = vec4(0.0);
            return;
        }

        vec2 worldPosition = vec2(
            a_localPosition.x * o0.x + a_localPosition.y * o0.z + o1.x,
            a_localPosition.x * o0.y + a_localPosition.y * o0.w + o1.y
        );
        gl_Position = u_mvp * vec4(worldPosition, o1.z, 1.0);
    }

    // Color/alpha is reconstructed from Cocos displayed state using the same
    // premultiply rule as CCSprite::updateColor(), so a stale suppressed quad
    // can no longer make an active sprite turn clear.
    t_color = resolvedColor;
}
