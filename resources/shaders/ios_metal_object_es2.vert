precision highp float;

attribute vec2 a_positionOffset;
attribute vec2 a_texCoord;
attribute float a_spriteIndex;

uniform sampler2D u_metalSpriteStateTexture;
uniform vec2 u_metalSpriteStateTextureSize;
uniform float u_metalSpriteStateNeedsYFlip;
uniform vec2 u_cameraPosition;
uniform vec2 u_cameraRight;
uniform vec2 u_cameraUp;
uniform vec2 u_cameraSize;

varying mediump vec2 v_texCoord;
varying lowp vec4 v_color;

vec2 stateUV(float texelIndex) {
    float x = mod(texelIndex, u_metalSpriteStateTextureSize.x);
    float y = floor(texelIndex / u_metalSpriteStateTextureSize.x);
    if (u_metalSpriteStateNeedsYFlip > 0.5)
        y = (u_metalSpriteStateTextureSize.y - 1.0) - y;
    return (vec2(x, y) + vec2(0.5)) / u_metalSpriteStateTextureSize;
}

void main() {
    float firstTexel = a_spriteIndex * 3.0;
    vec4 state0 = texture2D(u_metalSpriteStateTexture, stateUV(firstTexel));
    vec4 state1 = texture2D(u_metalSpriteStateTexture, stateUV(firstTexel + 1.0));
    vec4 state2 = texture2D(u_metalSpriteStateTexture, stateUV(firstTexel + 2.0));

    if (state2.z < 0.5) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_texCoord = a_texCoord;
        v_color = vec4(0.0);
        return;
    }

    mat2 localTransform = mat2(state0.zw, state1.xy);
    vec2 worldPosition = state0.xy + localTransform * a_positionOffset;

    // Use the same object-layer camera basis that Metal receives. This avoids
    // depending on whichever Cocos model-view matrix happened to be active
    // when the renderer's early-z child submitted its compute frame.
    vec2 cameraOffset = worldPosition - u_cameraPosition;
    vec2 screenPosition = vec2(
        dot(u_cameraRight, cameraOffset),
        dot(u_cameraUp, cameraOffset)
    );
    vec2 safeCameraSize = max(u_cameraSize, vec2(0.0001));
    vec2 clipPosition = (screenPosition / safeCameraSize) * 2.0 - 1.0;
    gl_Position = vec4(clipPosition, 0.0, 1.0);

    v_texCoord = a_texCoord;
    v_color = vec4(state1.zw, state2.xy);
}
