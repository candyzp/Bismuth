precision highp float;
attribute vec2 a_localPosition;
uniform mat4 u_projection;
uniform mat4 u_modelView;
uniform vec3 u_positions[4];
uniform vec2 u_uvs[4];
uniform vec4 u_colors[4];
varying vec2 t_texCoord;
varying vec4 t_color;
void main() {
    vec2 corner = a_localPosition;
    vec3 position = mix(mix(u_positions[0], u_positions[1], corner.x),
                        mix(u_positions[2], u_positions[3], corner.x), corner.y);
    t_texCoord = mix(mix(u_uvs[0], u_uvs[1], corner.x),
                     mix(u_uvs[2], u_uvs[3], corner.x), corner.y);
    t_color = mix(mix(u_colors[0], u_colors[1], corner.x),
                  mix(u_colors[2], u_colors[3], corner.x), corner.y);
    gl_Position = u_projection * u_modelView * vec4(position, 1.0);
}
