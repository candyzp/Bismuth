precision highp float;

// Static batch geometry is authored in GameObject-local space. Geometry Dash
// remains responsible for resolving the root object's final state; this shader
// only performs the repetitive per-vertex math in parallel.
attribute vec2 a_localPosition;
attribute vec2 a_texCoord;
attribute float a_objectStateIndex;
attribute float a_spriteStateIndex;

uniform sampler2D u_objectStateTexture;
uniform sampler2D u_spriteStateTexture;
uniform vec2 u_objectStateTextureSize;
uniform vec2 u_spriteStateTextureSize;
uniform mat4 u_mvp;

varying vec2 t_texCoord;
varying vec4 t_color;

const float PI = 3.1415926535897932384626433832795;

vec4 fetchData(sampler2D textureSampler, vec2 textureSize, float index) {
    float x = mod(index, textureSize.x);
    float y = floor(index / textureSize.x);
    return texture2D(textureSampler, (vec2(x, y) + vec2(0.5)) / textureSize);
}

void main() {
    float objectIndex = floor(a_objectStateIndex + 0.5);
    float objectBase = objectIndex * 2.0;
    vec4 o0 = fetchData(u_objectStateTexture, u_objectStateTextureSize, objectBase + 0.0);
    vec4 o1 = fetchData(u_objectStateTexture, u_objectStateTextureSize, objectBase + 1.0);

    // o0 = resolved position.xy, rotation, scaleX
    // o1 = scaleY, resolved root opacity, resolved visibility, dynamic marker
    if (o1.z < 0.5) {
        gl_Position = vec4(4.0, 4.0, 4.0, 1.0);
        t_texCoord = a_texCoord;
        t_color = vec4(0.0);
        return;
    }

    vec2 local = a_localPosition * vec2(o0.w, o1.x);
    float radians = -o0.z * PI / 180.0;
    float s = sin(radians);
    float c = cos(radians);
    vec2 rotated = vec2(
        local.x * c - local.y * s,
        local.x * s + local.y * c
    );

    vec2 worldPosition = o0.xy + rotated;
    gl_Position = u_mvp * vec4(worldPosition, 0.0, 1.0);
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
