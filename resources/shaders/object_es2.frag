precision mediump float;

uniform sampler2D u_spriteSheetTexture;

varying mediump vec2 v_texCoord;
varying mediump vec4 v_color;

void main() {
    gl_FragColor = texture2D(u_spriteSheetTexture, v_texCoord) * v_color;
}
