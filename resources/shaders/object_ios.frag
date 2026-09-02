precision mediump float;

uniform sampler2D u_spriteSheetTexture;

varying vec2 t_texCoord;
varying vec4 t_color;

void main() {
    gl_FragColor = texture2D(u_spriteSheetTexture, t_texCoord) * t_color;
}
