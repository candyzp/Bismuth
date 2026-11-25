// import BASIC_VERT from "./shaders/basic.vert";
// import BASIC_FRAG from "./shaders/basic.frag";

import { Transform, Vec2 } from "./math";

export const BASIC_VERT = 
`#version 300 es
 
in vec2 a_position;

uniform mat3 u_transform;
 
void main() {
    gl_Position = vec4((u_transform * vec3(a_position, 1.0)).xy, 0.0, 1.0);
}`;
export const BASIC_FRAG = 
`#version 300 es
 
precision highp float;

uniform vec4 u_color;

out vec4 outColor;

void main() {
    outColor = u_color;//vec4(u_color.rgb * u_color.a, u_color.a);
}`;

export const TEXTURE_VERT = 
`#version 300 es

layout (location=0) in vec2 a_position;
layout (location=1) in vec2 a_texCoord;

uniform mat3 u_transform;

out vec2 t_texCoord;
 
void main() {
    gl_Position = vec4((u_transform * vec3(a_position, 1.0)).xy, 0.0, 1.0);
    t_texCoord  = a_texCoord;
}`;
export const TEXTURE_FRAG = 
`#version 300 es
 
precision highp float;

in vec2 t_texCoord;
 
out vec4 outColor;

uniform sampler2D u_texture;
 
void main() {
    outColor = texture(u_texture, t_texCoord);
}`;

type VertexLayout = 'none' | 'pos2' | 'pos2-tex';

export class VertexBuffer {
    gfx: Graphics;
    data: number[] = [];
    layout: VertexLayout = 'none';
    buffer: WebGLBuffer | null = null;
    vao: WebGLVertexArrayObject | null = null;
    vertexCount: number = 0;

    constructor(gfx: Graphics) {
        this.gfx = gfx;
    }

    add(array: number[]) {
        this.data = this.data.concat(...array);
    }

    pos(pos: Vec2) {
        this.add([pos.x, pos.y]);
        this.layout = 'pos2';
        this.vertexCount++;
    }

    posTex(pos: Vec2, tex: Vec2) {
        this.add([pos.x, pos.y, tex.x, tex.y]);
        this.layout = 'pos2-tex';
        this.vertexCount++;
    }

    generate() {
        if (this.buffer != null) return;
        this.buffer = this.gfx.createStaticDrawBuffer(new Float32Array(this.data));
        const gl = this.gfx.gl;
        this.vao = gl.createVertexArray();
        gl.bindVertexArray(this.vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
        const stride = this.layout == 'pos2-tex' ? 16 : 8;
        gl.vertexAttribPointer(0, 2, gl.FLOAT, false, stride, 0);
        gl.enableVertexAttribArray(0);
        if (this.layout == 'pos2-tex') {
            gl.vertexAttribPointer(1, 2, gl.FLOAT, false, stride, 8);
            gl.enableVertexAttribArray(1);
        }
    }

    bind() { this.generate(); this.gfx.gl.bindVertexArray(this.vao); }
    drawTriangles() { this.bind(); this.gfx.gl.drawArrays(this.gfx.gl.TRIANGLES, 0, this.vertexCount); }
    drawLines() { this.bind(); this.gfx.gl.drawArrays(this.gfx.gl.LINES, 0, this.vertexCount); }
    drawLineStrip() { this.bind(); this.gfx.gl.drawArrays(this.gfx.gl.LINE_STRIP, 0, this.vertexCount); }
};

export class ShaderProgram {
    program: WebGLProgram;
    gl: WebGL2RenderingContext;

    compileShader(type: number, source: string): WebGLShader {
        const gl = this.gl;
        const shader = gl.createShader(type);
        if (shader == null)
            throw "shader == null";
        gl.shaderSource(shader, source);
        gl.compileShader(shader);
        const success = gl.getShaderParameter(shader, gl.COMPILE_STATUS);
        if (!success)
            throw (`could not compile ${type == gl.VERTEX_SHADER ? 'vertex' : 'fragment'} shader:${gl.getShaderInfoLog(shader)}`);
        return shader;
    }

    constructor(gl: WebGL2RenderingContext, vertexSource: string, fragmentSource: string) {
        this.gl = gl;
        this.program = gl.createProgram();
        gl.attachShader(this.program, this.compileShader(gl.VERTEX_SHADER, vertexSource));
        gl.attachShader(this.program, this.compileShader(gl.FRAGMENT_SHADER, fragmentSource));
        gl.linkProgram(this.program);
        const success = gl.getProgramParameter(this.program, gl.LINK_STATUS);
        if (!success)
            throw ("program failed to link:" + gl.getProgramInfoLog(this.program));
    }

    use() { this.gl.useProgram(this.program); }

    loc(name: string): WebGLUniformLocation | null { this.use(); return this.gl.getUniformLocation(this.program, name); }
    setFloat(name: string, value: number) { this.gl.uniform1f(this.loc(name), value); }
    setFloat2(name: string, x: number, y: number) { this.gl.uniform2f(this.loc(name), x, y); }
    setFloat3(name: string, x: number, y: number, z: number) { this.gl.uniform3f(this.loc(name), x, y, z); }
    setFloat4(name: string, x: number, y: number, z: number, w: number) { this.gl.uniform4f(this.loc(name), x, y, z, w); }

    setVec2(name: string, v: Vec2) { this.setFloat2(name, v.x, v.y); }
    setColor(name: string, color: Color) { this.setFloat4(name, color.r, color.g, color.b, color.a); }

    setMat3(name: string, mat: number[]) {
        this.use();
        this.gl.uniformMatrix3fv(this.loc(name), false, mat);
    }
    setTransform(name: string, transform: Transform) { this.setMat3(name, transform.data()); }
};

export interface Color { r: number, g: number, b: number, a: number };
export function Color(r: number, g: number, b: number, a: number = 1): Color {
    return { r, g, b, a };
}

function remap(a: number, s0: number, s1: number, d0: number, d1: number): number {
    return (a - s0) / (s1 - s0) * (d1 - d0) + d0;
}

export class Graphics {
    canvas: HTMLCanvasElement;
    gl: WebGL2RenderingContext;

    basicShader: ShaderProgram;
    textureShader: ShaderProgram;

    quad: VertexBuffer;

    cameraPos: Vec2    = Vec2(0, 0);
    cameraZoom: number = 1;

    width: number = 0;
    height: number = 0;

    constructor(elem: HTMLCanvasElement) {
        this.canvas = elem;
        this.gl = this.canvas.getContext('webgl2') as WebGL2RenderingContext;
        this.basicShader   = new ShaderProgram(this.gl, BASIC_VERT,   BASIC_FRAG);
        this.textureShader = new ShaderProgram(this.gl, TEXTURE_VERT, TEXTURE_FRAG);

        this.quad = new VertexBuffer(this);
        this.quad.posTex(Vec2(-0.5, -0.5), Vec2(0, 0));
        this.quad.posTex(Vec2(-0.5,  0.5), Vec2(0, 1));
        this.quad.posTex(Vec2( 0.5,  0.5), Vec2(1, 1));
        this.quad.posTex(Vec2(-0.5, -0.5), Vec2(0, 0));
        this.quad.posTex(Vec2( 0.5,  0.5), Vec2(1, 1));
        this.quad.posTex(Vec2( 0.5, -0.5), Vec2(1, 0));

        this.gl.enable(this.gl.BLEND);
        this.gl.blendFunc(this.gl.SRC_ALPHA, this.gl.ONE_MINUS_SRC_ALPHA);
    }

    get screenLeft(): number   { return this.cameraPos.x - this.height / this.width / this.cameraZoom; }
    get screenRight(): number  { return this.cameraPos.x + this.height / this.width / this.cameraZoom; }
    get screenTop(): number    { return this.cameraPos.y + 1 / this.cameraZoom; }
    get screenBottom(): number { return this.cameraPos.y - 1 / this.cameraZoom; }

    getViewTransform(): Transform {
        let mat = Transform.identity();
        mat = mat.scale(Vec2(this.height / this.width, 1).muln(this.cameraZoom));
        mat = mat.translate(this.cameraPos.neg());
        return mat;
    }

    canvasPosToNDC(p: Vec2): Vec2 {
        const rect = this.canvas.getBoundingClientRect();
        return Vec2(
                p.x / rect.width  * 2 - 1,
            1 - p.y / rect.height * 2
        );
    }

    canvasPosToWorldPos(p: Vec2): Vec2 {
        const rect = this.canvas.getBoundingClientRect();
        return Vec2(
            remap(p.x, 0, rect.width,  this.screenLeft, this.screenRight),
            remap(p.y, 0, rect.height, this.screenTop,  this.screenBottom),
        );
    }

    windowPosToWorldPos(p: Vec2): Vec2 {
        const rect = this.canvas.getBoundingClientRect();
        return this.canvasPosToWorldPos(p.sub(Vec2(rect.x, rect.y)));
    }

    viewport(width: number, height: number) {
        this.width = width;
        this.height = height;
        this.canvas.width  = width;
        this.canvas.height = height;
        this.canvas.style.width  = width  / window.devicePixelRatio + 'px';
        this.canvas.style.height = height / window.devicePixelRatio + 'px';
        this.gl.viewport(0, 0, width, height);
    }

    createStaticDrawBuffer(data: AllowSharedBufferSource): WebGLBuffer {
        const gl = this.gl;
        const buffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
        gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
        return buffer;
    }

    deleteBuffer(buffer: WebGLBuffer) {
        this.gl.deleteBuffer(buffer);
    }

    createTexture(image: HTMLImageElement): WebGLTexture {
        const gl = this.gl;
        const texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, image);
        return texture;
    }

    clear(color: Color) {
        const gl = this.gl;
        gl.clearColor(color.r, color.g, color.b, color.a);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }

    createShader(vert: string, frag: string): ShaderProgram {
        return new ShaderProgram(this.gl, vert, frag);
    }

    drawQuad() {
        this.quad.drawTriangles();
    }

    setTransformUniform(shader: ShaderProgram, transform: Transform = Transform.identity()) {
        shader.setVec2("u_resolution", Vec2(this.width, this.height))
        shader.setTransform("u_transform", transform.mul(this.getViewTransform()));
    }

    drawSolid(vbuf: VertexBuffer, color: Color, transform: Transform = Transform.identity()) {
        this.basicShader.use();
        this.setTransformUniform(this.basicShader, transform);
        this.basicShader.setColor("u_color", color);
        vbuf.drawTriangles();
    }

    drawTextured(vbuf: VertexBuffer, texture: WebGLTexture, transform: Transform = Transform.identity()) {
        const gl = this.gl;
        this.textureShader.use();
        this.setTransformUniform(this.textureShader, transform);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, texture);
        vbuf.drawTriangles();
    }

    drawRect(pos: Vec2, size: Vec2, color: Color) {
        let mat = Transform.identity();
        mat = mat.translate(pos.add(size.muln(0.5)));
        mat = mat.scale(size);
        this.drawSolid(this.quad, color, mat);
    }

    drawPath(path: Vec2[], color: Color) {
        let vbuf = new VertexBuffer(this);
        for (const p of path)
            vbuf.pos(p);
        
        this.basicShader.use();
        this.setTransformUniform(this.basicShader);
        this.basicShader.setColor("u_color", color);
        vbuf.drawLineStrip();
    }

    set lineWidth(w: number) {
        this.gl.lineWidth(w);
    }
};