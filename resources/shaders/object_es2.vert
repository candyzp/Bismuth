precision highp float;

attribute vec2 a_positionOffset;
attribute vec2 a_texCoord;
attribute vec2 a_objectAndGroupIndex;
attribute vec2 a_spriteAndMeta;
attribute vec2 a_objectStartPosition;

varying mediump vec2 v_texCoord;
varying mediump vec4 v_color;

uniform sampler2D u_objectDataTexture;
uniform sampler2D u_groupDataTexture;
uniform sampler2D u_byteDataTexture;

uniform vec2 u_objectDataTextureSize;
uniform vec2 u_groupDataTextureSize;
uniform vec2 u_byteDataTextureSize;

uniform float u_colorChannelsOffset;
uniform float u_objectHSVOffset;
uniform float u_pulseEntriesOffset;
uniform float u_spriteAppearanceOffset;
uniform float u_pulseEntryCount;

uniform mat4 u_mvp;
uniform float u_timer;
uniform float u_audioScale;
uniform vec2 u_cameraViewSize;
uniform vec2 u_cameraPosition;
uniform vec2 u_cameraRight;
uniform vec2 u_cameraUp;
uniform vec2 u_cameraSize;
uniform vec2 u_winSize;
uniform float u_screenRight;
uniform float u_cameraUnzoomedX;
uniform vec3 u_specialLightBGColor;
uniform float u_gameStateFlags;

const float PI = 3.14159265358979323846;

const float COLOR_CHANNEL_BG = 1000.0;
const float COLOR_CHANNEL_LBG = 1007.0;
const float COLOR_CHANNEL_BLACK = 1010.0;
const float COLOR_CHANNEL_LIGHTER = 1012.0;

const float SPRITE_TYPE_BASE = 0.0;
const float SPRITE_TYPE_DETAIL = 1.0;
const float SPRITE_TYPE_BLACK = 2.0;
const float SPRITE_TYPE_GLOW = 3.0;

const float OBJECT_FLAG_USES_AUDIO_SCALE = 1.0;
const float OBJECT_FLAG_CUSTOM_AUDIO_SCALE = 2.0;
const float OBJECT_FLAG_IS_ORB = 4.0;
const float OBJECT_FLAG_IS_INVISIBLE_BLOCK = 8.0;
const float OBJECT_FLAG_SPECIAL_GLOW_COLOR = 16.0;
const float OBJECT_FLAG_HAS_BASE_HSV = 64.0;
const float OBJECT_FLAG_HAS_DETAIL_HSV = 128.0;
const float OBJECT_FLAG_IS_STATIC_OBJECT = 256.0;

const float GAME_STATE_IS_PLAYER_DEAD = 1.0;

const float COLOR_DETAIL_FLAG = 4096.0;
const float COLOR_DIRECT_FLAG = 8192.0;

const float SPRITE_APPEARANCE_VISIBLE = 1.0;
const float SPRITE_APPEARANCE_OPACITY_MODIFY_RGB = 2.0;

vec4 fetchFloatTexel(sampler2D source, vec2 dimensions, float index) {
    float x = mod(index, dimensions.x);
    float y = floor(index / dimensions.x);
    return texture2D(source, (vec2(x, y) + vec2(0.5)) / dimensions);
}

vec4 fetchByteTexel(float index) {
    float x = mod(index, u_byteDataTextureSize.x);
    float y = floor(index / u_byteDataTextureSize.x);
    return texture2D(
        u_byteDataTexture,
        (vec2(x, y) + vec2(0.5)) / u_byteDataTextureSize
    );
}

vec4 decodeBytes(vec4 normalizedBytes) {
    return floor(normalizedBytes * 255.0 + 0.5);
}

float hasFlag(float flags, float bitValue) {
    return mod(floor(flags / bitValue), 2.0);
}

vec4 getColorChannel(float channelIndex) {
    return fetchByteTexel(u_colorChannelsOffset + channelIndex);
}

vec4 getObjectHSV(float objectIndex, bool isDetail) {
    float hsvIndex = u_objectHSVOffset + objectIndex * 2.0;
    if (isDetail)
        hsvIndex += 1.0;
    return decodeBytes(fetchByteTexel(hsvIndex));
}

vec4 getPulseFlags(float pulseIndex) {
    return decodeBytes(fetchByteTexel(u_pulseEntriesOffset + pulseIndex * 2.0));
}

vec4 getPulsePayload(float pulseIndex) {
    return decodeBytes(fetchByteTexel(u_pulseEntriesOffset + pulseIndex * 2.0 + 1.0));
}

vec4 getSpriteAppearance(float spriteIndex) {
    return decodeBytes(fetchByteTexel(u_spriteAppearanceOffset + spriteIndex * 2.0));
}

vec4 getSpriteAppearanceFlags(float spriteIndex) {
    return decodeBytes(fetchByteTexel(u_spriteAppearanceOffset + spriteIndex * 2.0 + 1.0));
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

vec3 applyHSVBytes(vec4 packedHSV, vec3 rgb) {
    float hueBits = packedHSV.b + mod(packedHSV.a, 2.0) * 256.0;
    float satAdd = mod(floor(packedHSV.a / 32.0), 2.0);
    float valAdd = mod(floor(packedHSV.a / 16.0), 2.0);

    float hue = (hueBits - 256.0) / 360.0;
    float saturation = packedHSV.g / 127.5;
    float value = packedHSV.r / 127.5;

    if (satAdd > 0.5)
        saturation -= 1.0;
    if (valAdd > 0.5)
        value -= 1.0;

    vec3 hsv = rgb2hsv(rgb);
    hsv.x = mod(hsv.x + hue, 1.0);
    if (satAdd > 0.5)
        hsv.y += saturation;
    else
        hsv.y *= saturation;
    if (valAdd > 0.5)
        hsv.z += value;
    else
        hsv.z *= value;

    hsv.y = clamp(hsv.y, 0.0, 1.0);
    hsv.z = clamp(hsv.z, 0.0, 1.0);
    return hsv2rgb(hsv);
}

vec4 applyPulseEntries(
    vec4 initialColor,
    float pulseStartIndex,
    bool isBaseColor
) {
    vec4 color = initialColor;

    for (float i = 0.0; i < u_pulseEntryCount; i += 1.0) {
        float pulseIndex = pulseStartIndex + i;
        vec4 flags = getPulseFlags(pulseIndex);

        if (dot(flags, vec4(1.0)) < 0.5)
            break;

        float value = (flags.r + flags.g * 256.0) / 65535.0;
        float colorChannel = flags.b + mod(flags.a, 4.0) * 256.0;

        float baseOnly = mod(floor(flags.a / 4.0), 2.0);
        float detailOnly = mod(floor(flags.a / 8.0), 2.0);
        float isHSV = mod(floor(flags.a / 16.0), 2.0);
        float isDynamicHSV = mod(floor(flags.a / 32.0), 2.0);
        float isActive = mod(floor(flags.a / 64.0), 2.0);

        if (isActive < 0.5 || value <= 0.0)
            continue;
        if (baseOnly > 0.5 && !isBaseColor)
            continue;
        if (detailOnly > 0.5 && isBaseColor)
            continue;

        vec4 payload = getPulsePayload(pulseIndex);
        if (isHSV > 0.5) {
            vec3 sourceColor = color.rgb;
            if (colorChannel > 0.5)
                sourceColor = getColorChannel(colorChannel).rgb;

            vec4 hsv = payload;
            if (isDynamicHSV > 0.5) {
                float satAdd = mod(floor(hsv.a / 32.0), 2.0);
                float valAdd = mod(floor(hsv.a / 16.0), 2.0);

                // Match the owner's getMultipliedHSV(): hue is scaled toward
                // zero while additive/multiplicative saturation and value keep
                // their respective interpolation rules.
                float hueBits = hsv.b + mod(hsv.a, 2.0) * 256.0;
                float hue = (hueBits - 256.0) / 360.0;
                float scaledHueBits = clamp(
                    floor(hue * value * 360.0 + 256.0 + 0.5),
                    0.0,
                    511.0
                );
                hsv.b = mod(scaledHueBits, 256.0);
                hsv.a = floor(hsv.a / 2.0) * 2.0 +
                    floor(scaledHueBits / 256.0);

                float sat = hsv.g / 127.5;
                float val = hsv.r / 127.5;
                if (satAdd > 0.5) {
                    sat = (sat - 1.0) * value;
                } else {
                    sat = sat * value + (1.0 - value);
                }
                if (valAdd > 0.5) {
                    val = (val - 1.0) * value;
                } else {
                    val = val * value + (1.0 - value);
                }
                hsv.g = (satAdd > 0.5 ? sat + 1.0 : sat) * 127.5;
                hsv.r = (valAdd > 0.5 ? val + 1.0 : val) * 127.5;
            }

            vec3 destinationColor = applyHSVBytes(hsv, sourceColor);
            if (value >= 1.0)
                color.rgb = destinationColor;
            else
                color.rgb = mix(color.rgb, destinationColor, value);
        } else {
            vec3 destinationColor = payload.rgb / 255.0;
            color.rgb = mix(color.rgb, destinationColor, value);
        }
    }

    return color;
}

vec2 invisibleBlockOpacity(vec2 objectPosition, float fadeMargin) {
    float centerLeftX = u_winSize.x * 0.5 - 75.0;
    float centerRightX = centerLeftX + 110.0;
    float someScreenLeft = u_screenRight - centerRightX - 90.0;

    float fadePosX = objectPosition.x;
    if (fadePosX <= u_cameraUnzoomedX)
        fadePosX += fadeMargin;
    else
        fadePosX -= fadeMargin;

    float result = u_cameraViewSize.x * 0.5;
    if (fadePosX > result + u_cameraPosition.x)
        result = (result - (fadePosX - u_cameraPosition.x - result)) * 0.014285714;
    else
        result = (result - (result + u_cameraPosition.x - fadePosX)) * 0.02;
    float relMod = clamp(result, 0.0, 1.0);

    float someWidth1;
    if (fadePosX <= centerRightX + u_cameraPosition.x)
        someWidth1 = (centerLeftX + u_cameraPosition.x - fadePosX) /
            max(centerLeftX - 30.0, 1.0);
    else
        someWidth1 = (fadePosX - u_cameraPosition.x - centerRightX) /
            max(someScreenLeft, 1.0);

    someWidth1 = clamp(someWidth1, 0.0, 1.0);
    return vec2(
        min(someWidth1 * 0.95 + 0.05, relMod),
        min(someWidth1 * 0.85 + 0.15, relMod)
    );
}

float calculateAudioScale(float objectFlags, vec4 object0) {
    float scale = u_audioScale;
    if (hasFlag(objectFlags, OBJECT_FLAG_CUSTOM_AUDIO_SCALE) > 0.5)
        scale = (u_audioScale - 0.1) * (object0.w - object0.z) + object0.z;
    if (hasFlag(objectFlags, OBJECT_FLAG_IS_ORB) > 0.5)
        scale = min(scale + 0.3, 1.2);
    return scale;
}

void main() {
    float objectIndex = floor(a_objectAndGroupIndex.x + 0.5);
    float groupIndex = floor(a_objectAndGroupIndex.y + 0.5);
    float spriteIndex = floor(a_spriteAndMeta.x + 0.5);

    float encodedMeta = floor(a_spriteAndMeta.y + 0.5);
    float spriteType = floor(encodedMeta / 65536.0);
    float colorChannelFlags = mod(encodedMeta, 65536.0);
    float colorChannel = mod(colorChannelFlags, 4096.0);
    float directTint = hasFlag(colorChannelFlags, COLOR_DIRECT_FLAG);
    float isDetail = hasFlag(colorChannelFlags, COLOR_DETAIL_FLAG);

    float objectBase = objectIndex * 3.0;
    vec4 object0 = fetchFloatTexel(
        u_objectDataTexture, u_objectDataTextureSize, objectBase
    );
    vec4 object1 = fetchFloatTexel(
        u_objectDataTexture, u_objectDataTextureSize, objectBase + 1.0
    );
    float objectFlags = object0.y;

    float groupBase = groupIndex * 3.0;
    vec4 group0 = fetchFloatTexel(
        u_groupDataTexture, u_groupDataTextureSize, groupBase
    );
    vec4 group1 = fetchFloatTexel(
        u_groupDataTexture, u_groupDataTextureSize, groupBase + 1.0
    );
    vec4 group2 = fetchFloatTexel(
        u_groupDataTexture, u_groupDataTextureSize, groupBase + 2.0
    );

    vec2 objectPosition = mat2(group0.xy, group0.zw) * a_objectStartPosition + group2.xy;

    // Match the owner's conservative object-anchor camera culling. All vertices
    // of this sprite share the same object/group metadata.
    vec2 cameraOffset = objectPosition - u_cameraPosition;
    vec2 screenPosition = vec2(
        dot(u_cameraRight, cameraOffset),
        dot(u_cameraUp, cameraOffset)
    );
    const float cullMargin = 100.0;
    if (screenPosition.x < -cullMargin ||
        screenPosition.x > u_cameraSize.x + cullMargin ||
        screenPosition.y < -cullMargin ||
        screenPosition.y > u_cameraSize.y + cullMargin) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_texCoord = a_texCoord;
        v_color = vec4(0.0);
        return;
    }

    mat2 localTransform = mat2(1.0);
    if (hasFlag(objectFlags, OBJECT_FLAG_IS_STATIC_OBJECT) < 0.5)
        localTransform = mat2(group1.xy, group1.zw);

    if (abs(object0.x) > 0.00001) {
        float angle = -object0.x * u_timer / 180.0 * PI;
        float s = sin(angle);
        float c = cos(angle);
        localTransform = mat2(c, s, -s, c) * localTransform;
    }

    if (hasFlag(objectFlags, OBJECT_FLAG_USES_AUDIO_SCALE) > 0.5) {
        float audioScale = calculateAudioScale(objectFlags, object0);
        localTransform = mat2(audioScale, 0.0, 0.0, audioScale) * localTransform;
    }

    vec2 vertexPosition = objectPosition + localTransform * a_positionOffset;
    gl_Position = u_mvp * vec4(vertexPosition, 0.0, 1.0);
    v_texCoord = a_texCoord;

    vec4 appearance = getSpriteAppearance(spriteIndex);
    vec4 appearanceFlags = getSpriteAppearanceFlags(spriteIndex);
    float visible = mod(appearanceFlags.r, 2.0);
    float opacityModifyRGB = mod(floor(appearanceFlags.r / 2.0), 2.0);
    if (visible < 0.5) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_color = vec4(0.0);
        return;
    }

    vec4 color;
    if (directTint > 0.5) {
        // texture2D returns normalized RGBA; the packed CPU bytes are already
        // converted to 0..1 by the GL sampler. Alpha is applied below exactly
        // once through directSpriteOpacity.
        color = vec4(appearance.rgb, 1.0);
    } else {
        color = getColorChannel(colorChannel);
    }

    if (colorChannel == COLOR_CHANNEL_LIGHTER && directTint < 0.5) {
        color = getColorChannel(object1.z);
        vec3 hsv = rgb2hsv(color.rgb);
        hsv.y *= 0.65;
        hsv.z = clamp(hsv.z + 0.15, 0.0, 1.0);
        color.rgb = hsv2rgb(hsv);
    }

    if (spriteType == SPRITE_TYPE_GLOW &&
        hasFlag(objectFlags, OBJECT_FLAG_SPECIAL_GLOW_COLOR) > 0.5) {
        color = vec4(u_specialLightBGColor, 1.0);
    }

    float pulseIndex = group2.w;
    if (directTint < 0.5 &&
        pulseIndex > 0.0 &&
        (spriteType == SPRITE_TYPE_BASE || spriteType == SPRITE_TYPE_DETAIL)) {
        color = applyPulseEntries(color, pulseIndex, spriteType == SPRITE_TYPE_BASE);
    }

    if (directTint < 0.5 && spriteType == SPRITE_TYPE_BASE &&
        hasFlag(objectFlags, OBJECT_FLAG_HAS_BASE_HSV) > 0.5) {
        color.rgb = applyHSVBytes(
            getObjectHSV(objectIndex, false),
            color.rgb
        );
    } else if (directTint < 0.5 && spriteType == SPRITE_TYPE_DETAIL &&
        hasFlag(objectFlags, OBJECT_FLAG_HAS_DETAIL_HSV) > 0.5) {
        color.rgb = applyHSVBytes(
            getObjectHSV(objectIndex, true),
            color.rgb
        );
    }

    if (hasFlag(objectFlags, OBJECT_FLAG_IS_INVISIBLE_BLOCK) > 0.5) {
        if (hasFlag(u_gameStateFlags, GAME_STATE_IS_PLAYER_DEAD) > 0.5) {
            if (spriteType == SPRITE_TYPE_GLOW)
                color.rgb = u_specialLightBGColor;
        } else {
            vec2 opacity = invisibleBlockOpacity(objectPosition, object1.x);
            if (spriteType != SPRITE_TYPE_GLOW) {
                color.a *= opacity.x;
            } else {
                vec3 colorBG = getColorChannel(COLOR_CHANNEL_BG).rgb;
                vec3 colorLBG = getColorChannel(COLOR_CHANNEL_LBG).rgb;
                vec3 colorB = (colorBG.r + colorBG.g + colorBG.b) >= (150.0 / 255.0)
                    ? vec3(1.0) : colorLBG;
                vec3 glowColor = opacity.x <= 0.8
                    ? u_specialLightBGColor
                    : mix(
                        colorB,
                        u_specialLightBGColor,
                        (1.0 - (opacity.x - 0.8) / 0.2) * 0.3 + 0.7
                    );
                color.rgb = glowColor;
                color.a *= opacity.y;
            }
        }
    }

    float externalOpacity = group2.z * object1.y;
    // Normal owner color-channel entries already carry channel opacity.
    // The per-sprite displayed alpha is used only for direct-tint mode, where
    // no valid channel entry exists (legacy/default color channel zero).
    float directSpriteOpacity = directTint > 0.5
        ? appearance.a
        : 1.0;
    color.a *= directSpriteOpacity * externalOpacity;

    // Direct sprite colors from legacy/default channel 0 are already displayed
    // tints. Respect Cocos' premultiplied-alpha flag instead of multiplying the
    // sprite alpha twice; ordinary channels are premultiplied after all effects.
    if (directTint > 0.5 && opacityModifyRGB > 0.5)
        color.rgb *= externalOpacity;
    else
        color.rgb *= color.a;

    if (color.a <= 0.001) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_color = vec4(0.0);
        return;
    }

    v_color = color;
}
