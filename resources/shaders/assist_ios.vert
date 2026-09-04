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
    float objectIndex = floor(a_objectStateIndex + 0.5);
    float objectBase = objectIndex * 2.0;
    vec4 o0 = fetchData(u_objectStateTexture, u_objectStateTextureSize, objectBase + 0.0);
    vec4 o1 = fetchData(u_objectStateTexture, u_objectStateTextureSize, objectBase + 1.0);

    // Stock affine matrix includes separate axis rotations, skew and anchor.
    // o0 = a,b,c,d; o1 = tx,ty,vertexZ,visibility.
    if (o1.w < 0.5) {
        gl_Position = vec4(4.0, 4.0, 4.0, 1.0);
        t_texCoord = a_texCoord;
        t_color = vec4(0.0);
        return;
    }

    vec2 worldPosition = vec2(
        a_localPosition.x * o0.x + a_localPosition.y * o0.z + o1.x,
        a_localPosition.x * o0.y + a_localPosition.y * o0.w + o1.y
    );
    gl_Position = u_mvp * vec4(worldPosition, o1.z, 1.0);
    t_texCoord = a_texCoord;

    float spriteIndex = floor(a_spriteStateIndex + 0.5);
    float spriteBase = spriteIndex * 3.0;
    vec4 resolvedColor = fetchData(u_spriteStateTexture, u_spriteStateTextureSize, spriteBase + 0.0);
    vec4 spriteMeta = fetchData(u_spriteStateTexture, u_spriteStateTextureSize, spriteBase + 2.0);

    // Bit 0 of the packed sprite flags is GD's final child visibility. The
    // shader consumes that result instead of trying to reproduce animation or
    // child-hierarchy visibility rules.
    float visibleBit = mod(floor(spriteMeta.x), 2.0);
    if (visibleBit < 0.5) {
        gl_Position = vec4(4.0, 4.0, 4.0, 1.0);
        t_color = vec4(0.0);
        return;
    }

    // Sprite color/alpha is already GD-resolved. Do not rebuild color channels,
    // HSV rules, Area Fade, portal state, or other appearance semantics here.
    t_color = resolvedColor;
}
