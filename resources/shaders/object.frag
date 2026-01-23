uniform sampler2D u_spriteSheetTexture;

in vec2 t_texCoord;
in vec4 t_color;

out vec4 FragColor;

void main() {
    FragColor = texture(u_spriteSheetTexture, t_texCoord) * t_color;
}