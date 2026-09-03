precision highp float;

attribute vec2 a_positionOffset;
attribute vec2 a_texCoord;
attribute float a_srbIndex;
attribute float a_colorChannel;

varying vec2 t_texCoord;
varying vec4 t_color;

uniform sampler2D u_staticDataTexture;
uniform sampler2D u_groupDataTexture;
uniform sampler2D u_colorDataTexture;
uniform vec2 u_staticDataTextureSize;
uniform vec2 u_groupDataTextureSize;
uniform vec2 u_colorDataTextureSize;
uniform float u_runtimeDataOffset;

uniform mat4 u_mvp;
uniform float u_timer;
uniform float u_audioScale;
uniform vec2 u_cameraPosition;
uniform vec2 u_cameraViewSize;
uniform vec2 u_winSize;
uniform float u_screenRight;
uniform float u_cameraUnzoomedX;
uniform vec3 u_specialLightBGColor;
uniform float u_gameStateFlags;
uniform float u_spriteSheet;

const float PI = 3.1415926535897932384626433832795;
const float COLOR_CHANNEL_BG = 1000.0;
const float COLOR_CHANNEL_LBG = 1007.0;
const float SPRITE_SHEET_GLOW = 5.0;

vec4 fetchData(sampler2D textureSampler, vec2 textureSize, float index) {
    float x = mod(index, textureSize.x);
    float y = floor(index / textureSize.x);
    return texture2D(textureSampler, (vec2(x, y) + vec2(0.5)) / textureSize);
}

float hasFlag(float flags, float bitValue) {
    return mod(floor(flags / bitValue), 2.0);
}

vec2 rotatePointAroundOrigin(vec2 point, float angleInRadians) {
    float rotSin = sin(angleInRadians);
    float rotCos = cos(angleInRadians);
    return vec2(
        point.x * rotCos - point.y * rotSin,
        point.x * rotSin + point.y * rotCos
    );
}

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

vec4 applyHSV(vec4 hsvInfo, vec4 color, vec2 addFlags) {
    vec3 hsv = rgb2hsv(color.rgb);
    hsv.x = mod(hsv.x + hsvInfo.x, 1.0);
    if (addFlags.x > 0.5)
        hsv.y += hsvInfo.y - 1.0;
    else
        hsv.y *= hsvInfo.y;
    if (addFlags.y > 0.5)
        hsv.z += hsvInfo.z - 1.0;
    else
        hsv.z *= hsvInfo.z;
    hsv.y = clamp(hsv.y, 0.0, 1.0);
    hsv.z = clamp(hsv.z, 0.0, 1.0);
    return vec4(hsv2rgb(hsv), color.a);
}

float getRelativeMod(float xPos, float left, float right, float offset) {
    float result = u_cameraViewSize.x * 0.5;
    if (xPos > result + u_cameraPosition.x)
        result = (result - (xPos - offset - u_cameraPosition.x - result)) * right;
    else
        result = (result - (result + u_cameraPosition.x - xPos - offset)) * left;
    return clamp(result, 0.0, 1.0);
}

vec2 calculateInvisibleBlockOpacity(vec2 objectPosition, float fadeMargin) {
    float centerLeftX = u_winSize.x * 0.5 - 75.0;
    float centerRightX = centerLeftX + 110.0;
    float someScreenLeft = u_screenRight - centerRightX - 90.0;

    float fadePosX = objectPosition.x;
    if (fadePosX <= u_cameraUnzoomedX)
        fadePosX += fadeMargin;
    else
        fadePosX -= fadeMargin;

    float relMod = getRelativeMod(fadePosX, 0.02, 0.014285714, 0.0);
    float someWidth1;
    if (fadePosX <= centerRightX + u_cameraPosition.x)
        someWidth1 = (centerLeftX + u_cameraPosition.x - fadePosX) / max(centerLeftX - 30.0, 1.0);
    else
        someWidth1 = (fadePosX - u_cameraPosition.x - centerRightX) / max(someScreenLeft, 1.0);

    someWidth1 = clamp(someWidth1, 0.0, 1.0);
    return vec2(
        min(someWidth1 * 0.95 + 0.05, relMod),
        min(someWidth1 * 0.85 + 0.15, relMod)
    );
}

void main() {
    float objectIndex = floor(a_srbIndex + 0.5);
    float objectBase = objectIndex * 5.0;
    vec4 s0 = fetchData(u_staticDataTexture, u_staticDataTextureSize, objectBase + 0.0);
    vec4 s1 = fetchData(u_staticDataTexture, u_staticDataTextureSize, objectBase + 1.0);
    vec4 s2 = fetchData(u_staticDataTexture, u_staticDataTextureSize, objectBase + 2.0);
    vec4 s3 = fetchData(u_staticDataTexture, u_staticDataTextureSize, objectBase + 3.0);
    vec4 s4 = fetchData(u_staticDataTexture, u_staticDataTextureSize, objectBase + 4.0);

    // Runtime state shares the static object texture so the ES2 backend does
    // not consume another vertex texture unit. Three contiguous texels per
    // object are refreshed each frame: r0/r1 hold raw Move/Rotate/Scale data,
    // while r2 is appearance-side state. Area Fade currently occupies r2.x;
    // the remaining components are reserved for Area Tint/related effects.
    float runtimeBase = u_runtimeDataOffset + objectIndex * 3.0;
    vec4 r0 = fetchData(u_staticDataTexture, u_staticDataTextureSize, runtimeBase + 0.0);
    vec4 r1 = fetchData(u_staticDataTexture, u_staticDataTextureSize, runtimeBase + 1.0);
    vec4 r2 = fetchData(u_staticDataTexture, u_staticDataTextureSize, runtimeBase + 2.0);
    float runtimeRotation = (r0.z + r0.w) * 0.5;
    vec2 runtimeScale = vec2(1.0) + r1.xy * r1.zw;
    float runtimeFadeOpacity = clamp(r2.x, 0.0, 1.0);

    vec2 objectPosition = s0.xy;
    float rotationSpeed = s0.z;
    float objectOpacity = s0.w;
    float audioScaleMin = s1.x;
    float audioScaleMax = s1.y;
    float fadeMargin = s1.z;
    float groupIndex = floor(s1.w + 0.5);
    float objectFlags = floor(s2.x + 0.5);

    float groupBase = groupIndex * 3.0;
    vec4 g0 = fetchData(u_groupDataTexture, u_groupDataTextureSize, groupBase + 0.0);
    vec4 g1 = fetchData(u_groupDataTexture, u_groupDataTextureSize, groupBase + 1.0);
    vec4 g2 = fetchData(u_groupDataTexture, u_groupDataTextureSize, groupBase + 2.0);

    mat2 positionalTransform = mat2(g0.x, g0.y, g0.z, g0.w);
    mat2 localTransform = mat2(g1.x, g1.y, g1.z, g1.w);
    objectPosition = positionalTransform * objectPosition + g2.xy;

    // GD calculates 2.2 Area / enter effect state on the CPU, while the GPU
    // performs the repetitive per-vertex transform and appearance work.
    objectPosition += r0.xy;
    objectOpacity *= g2.z;
    objectOpacity *= runtimeFadeOpacity;

    vec2 vertexOffset = a_positionOffset;
    if (hasFlag(objectFlags, 128.0) < 0.5)
        vertexOffset = localTransform * vertexOffset;

    // Runtime Area Scale and Area Rotate are local object transforms. Apply
    // them after the group transform, matching GD's visual-update ordering.
    vertexOffset *= runtimeScale;
    if (abs(runtimeRotation) > 0.00001)
        vertexOffset = rotatePointAroundOrigin(vertexOffset, -runtimeRotation / 180.0 * PI);

    if (abs(rotationSpeed) > 0.00001)
        vertexOffset = rotatePointAroundOrigin(vertexOffset, -rotationSpeed * u_timer / 180.0 * PI);

    if (hasFlag(objectFlags, 1.0) > 0.5) {
        float scaleValue = u_audioScale;
        if (hasFlag(objectFlags, 2.0) > 0.5)
            scaleValue = (u_audioScale - 0.1) * (audioScaleMax - audioScaleMin) + audioScaleMin;
        if (hasFlag(objectFlags, 4.0) > 0.5)
            scaleValue = min(scaleValue + 0.3, 1.2);
        vertexOffset *= scaleValue;
    }

    gl_Position = u_mvp * vec4(objectPosition + vertexOffset, 0.0, 1.0);
    t_texCoord = a_texCoord;

    float detailFlag = step(4096.0, a_colorChannel);
    float colorChannel = floor(a_colorChannel - detailFlag * 4096.0 + 0.5);
    t_color = fetchData(u_colorDataTexture, u_colorDataTextureSize, colorChannel);

    if (abs(u_spriteSheet - SPRITE_SHEET_GLOW) < 0.5 && hasFlag(objectFlags, 16.0) > 0.5)
        t_color = vec4(u_specialLightBGColor, 1.0);

    if (hasFlag(objectFlags, 8.0) > 0.5) {
        vec2 opacity = calculateInvisibleBlockOpacity(objectPosition, fadeMargin);
        if (abs(u_spriteSheet - SPRITE_SHEET_GLOW) > 0.5) {
            t_color.a *= opacity.x;
        } else {
            vec3 colorBG = fetchData(u_colorDataTexture, u_colorDataTextureSize, COLOR_CHANNEL_BG).rgb;
            vec3 colorLBG = fetchData(u_colorDataTexture, u_colorDataTextureSize, COLOR_CHANNEL_LBG).rgb;
            vec3 colorB = (colorBG.r + colorBG.g + colorBG.b) >= (150.0 / 255.0) ? vec3(1.0) : colorLBG;
            vec3 glowColor = opacity.x <= 0.8
                ? u_specialLightBGColor
                : mix(colorB, u_specialLightBGColor, (1.0 - (opacity.x - 0.8) / 0.2) * 0.3 + 0.7);
            t_color = vec4(glowColor, t_color.a * opacity.y);
        }
    }

    if (detailFlag < 0.5 && hasFlag(objectFlags, 32.0) > 0.5) {
        t_color = applyHSV(vec4(s2.y, s2.z, s2.w, 0.0), t_color, s3.xy);
    } else if (detailFlag > 0.5 && hasFlag(objectFlags, 64.0) > 0.5) {
        t_color = applyHSV(vec4(s3.z, s3.w, s4.x, 0.0), t_color, s4.yz);
    }

    t_color.a *= objectOpacity;
    if (t_color.a < 0.01) {
        gl_Position = vec4(5.0, 5.0, 5.0, 1.0);
        return;
    }

    t_color.rgb *= t_color.a;
}