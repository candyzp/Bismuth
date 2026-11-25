import { convexPartition, isClockwise } from "poly-partition";
import { BASIC_VERT, Color, Graphics, ShaderProgram } from "./graphics";
import { Transform, Vec2 } from "./math";
import { PolygonEditor } from "./polygon-editor";
import { Spritesheet } from "./spritesheet";

const CHECKERBOARD_FRAG = 
`#version 300 es
 
precision highp float;
 
out vec4 outColor;

uniform vec4 u_color1;
uniform vec4 u_color2;

uniform float u_checkerSize;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy / u_checkerSize);
    if ((int(p.x) + int(p.y)) % 2 == 0)
        outColor = u_color1;
    else
        outColor = u_color2;
}`;

const SCROLL_SENSITIVITY = 0.002;

const CHECKERBOARD_COLOR1 = Color(0.7, 0.7, 0.7);
const CHECKERBOARD_COLOR2 = Color(0.5, 0.5, 0.5);
const CHECKERBOARD_SIZE   = 16;

const POINT_SIZE = 12;

function createSetting(input: HTMLElement, name: string) {
    const label = document.createElement('label');
    input.id = name;
    label.setAttribute('for', name);
    label.textContent = name;

    const div = document.createElement('div');
    div.className = 'setting';
    div.appendChild(label);
    div.appendChild(input);

    document.querySelector('#app')?.appendChild(div);
}

class BooleanSetting {
    elem: HTMLInputElement;

    constructor(name: string, defValue: boolean) {
        this.elem = document.createElement('input');
        this.elem.type = 'checkbox';
        this.elem.checked = defValue;
        createSetting(this.elem, name);
    }

    get value(): boolean { return this.elem.checked; }
};

class EnumSetting {
    elem: HTMLSelectElement;
    values: (string | number)[];

    constructor(name: string, values: (string | number)[]) {
        this.values = values;
        this.elem = document.createElement('select');
        let i = 0;
        for (const val of values) {
            const opt = document.createElement('option');
            opt.value       = '' + i;
            opt.textContent = '' + val;
            this.elem.appendChild(opt);
            i++;
        }
        createSetting(this.elem, name);
    }

    get value(): string | number { return this.values[+this.elem.value]; }
};

function polygonFromString(str: string): Vec2[] {
    const ret: Vec2[] = [];
    for (const p of JSON.parse(str))
        ret.push(Vec2(p[0], p[1]));
    return ret;
}

function polygonToString(polygon: Vec2[]): string {
    const array: [number, number][] = [];
    for (const p of polygon)
        array.push([p.x, p.y]);
    return JSON.stringify(array);
}

export function ensureCounterClockwise(polygon: Vec2[]): Vec2[] {
    if (isClockwise(polygon)) {
        const oldPolygon = polygon;
        polygon = [];
        for (let i = oldPolygon.length - 1; i >= 0; i--)
            polygon.push(oldPolygon[i]);
    }
    return polygon;
}

export class Editor {
    gfx: Graphics;
    canvas: HTMLCanvasElement;
    
    spritesheet: Spritesheet | null = null;

    checkerShader: ShaderProgram;

    currentSprite: string = "";
    currentSpriteSize: Vec2 = Vec2(0, 0);

    currentPolygon: Vec2[] = [];

    polygonEditor = new PolygonEditor(this);

    shouldAlignPoints  = new BooleanSetting('Should align points to other points', true);
    showPolygon        = new BooleanSetting('Show polygon', true);
    shouldPreview      = new BooleanSetting('Preview', false);
    pointGridAlignment = new EnumSetting('Point grid alignment', ['none', 1, 0.5, 0.25, 0.125]);

    spriteListElements: { [_: string]: HTMLElement } = {};

    polygonShouldBeSaved = false;

    constructor(canvas: HTMLCanvasElement) {
        this.gfx = new Graphics(canvas);
        this.canvas = canvas;

        this.gfx.viewport(700, 700);

        const loop = () => {
            this.render();
            requestAnimationFrame(loop);
        }
        requestAnimationFrame(loop);

        this.addCameraControls();

        Spritesheet.load(this.gfx, "GJ_GameSheet-uhd")
            .then(ss => {
                this.spritesheet = ss;
                this.spritesheetLoaded();
            });

        this.checkerShader = this.gfx.createShader(BASIC_VERT, CHECKERBOARD_FRAG);

        this.canvas.addEventListener('mousedown', e => this.onMouseInput(e.button, this.gfx.windowPosToWorldPos(Vec2(e.clientX, e.clientY)), true));
        this.canvas.addEventListener('mouseup',   e => this.onMouseInput(e.button, this.gfx.windowPosToWorldPos(Vec2(e.clientX, e.clientY)), false));
        this.canvas.addEventListener('mousemove', e => this.onMouseMove(this.gfx.windowPosToWorldPos(Vec2(e.clientX, e.clientY))));

        this.canvas.addEventListener('contextmenu', e => e.preventDefault());

        const button = document.createElement('button');
        button.textContent = 'Print meshes';
        button.onclick = () => this.saveMeshes();
        document.querySelector('#app')?.appendChild(button);
    }

    spritesheetLoaded() {
        if (!this.spritesheet) return;

        for (const frame of this.spritesheet.frameNames) {
            const elem = document.createElement('div');
            elem.className = 'elem';
            elem.textContent = frame;
            elem.onclick = () => this.selectSprite(frame);
            document.querySelector("#sprite-list")?.appendChild(elem);
            this.spriteListElements[frame] = elem;
        }

        this.selectSprite("block003_color_04_001.png");
    }

    onMouseInput(button: number, pos: Vec2, pressed: boolean) {
        if (button == 1) return;

        this.polygonEditor.onMouseInput(button, pos, pressed);
    }

    saveSprite() {
        localStorage.setItem(this.currentSprite, polygonToString(this.currentPolygon));
    }

    saveMeshes() {
        if (!this.spritesheet) return;
        const out: { [_: string]: [number, number][][] } = {};

        for (let i = 0; i < localStorage.length; i++) {
            const key = localStorage.key(i);
            if (key == null) continue;
            const value = localStorage.getItem(key);
            if (value == null) continue;

            const polygon = polygonFromString(value);
            const convexesOut: [number, number][][] = [];

            const frame = this.spritesheet.getFrame(key);

            const convexes = convexPartition(ensureCounterClockwise(polygon));

            for (const contour of convexes) {
                if (isClockwise(contour))
                    console.log("CONTOUR CLOCKWISE!");

                const conv: [number, number][] = [];
                for (const point of contour) {
                    conv.push([
                        point.x / frame.rect.w,
                        point.y / frame.rect.h,
                    ]);
                }
                convexesOut.push(conv);
            }

            out[key] = convexesOut;
        }

        console.log(out);
        console.log(JSON.stringify(out, null, 4));
    }

    onMouseMove(pos: Vec2) {
        this.polygonEditor.onMouseMove(pos);
    }

    get winSize(): Vec2 { return Vec2(this.canvas.width, this.canvas.height); }

    render() {
        this.gfx.clear(Color(0.3, 0.3, 0.3));

        if (!this.spritesheet || this.currentSprite == "")
            return;

        this.drawCheckerboardRect(Vec2(0, 0), this.currentSpriteSize);
        if (this.shouldPreview.value)
            this.gfx.drawTextured(this.spritesheet.createMeshSpriteVertexBuffer(this.currentSprite, this.polygonEditor.polygon), this.spritesheet.texture);
        else
            this.gfx.drawTextured(this.spritesheet.createSpriteVertexBuffer(this.currentSprite), this.spritesheet.texture);

        if (this.showPolygon.value)
            this.polygonEditor.render();
    }

    drawPoint(point: Vec2, color: Color) {
        const size = POINT_SIZE / this.gfx.cameraZoom / this.winSize.y * 2;
        this.gfx.drawRect(point.subn(size / 2), Vec2(size, size), color);
    }

    isHoveringOverPoint(point: Vec2, mouse: Vec2): boolean {
        const size = POINT_SIZE / this.gfx.cameraZoom / this.winSize.y;
        const dx = Math.abs(point.x - mouse.x);
        const dy = Math.abs(point.y - mouse.y);
        return dx < size && dy < size;
    }

    selectSprite(name: string) {
        if (!this.spritesheet) return;

        const frame = this.spritesheet.getFrame(name);
        if (!frame) return;

        if (this.currentSprite != '') {
            this.spriteListElements[this.currentSprite].classList.remove('selected');
        
            if (this.polygonShouldBeSaved)
                this.saveSprite();
        }
        this.spriteListElements[name].classList.add('selected');

        this.currentSprite = name;
        this.currentSpriteSize = Vec2(frame.rect.w, frame.rect.h);

        this.gfx.cameraPos = this.currentSpriteSize.divn(2);
        this.gfx.cameraZoom = Math.min(2 / (this.currentSpriteSize.y * 1.2), 2 / (this.currentSpriteSize.x * 1.2));

        const str = localStorage.getItem(name);
        if (str) {
            this.currentPolygon = polygonFromString(str);
            this.polygonShouldBeSaved = true;
        } else {
            this.currentPolygon = [];
            this.currentPolygon.push(this.currentSpriteSize.mul(Vec2(0, 0)));
            this.currentPolygon.push(this.currentSpriteSize.mul(Vec2(0, 1)));
            this.currentPolygon.push(this.currentSpriteSize.mul(Vec2(1, 1)));
            this.currentPolygon.push(this.currentSpriteSize.mul(Vec2(1, 0)));
            this.polygonShouldBeSaved = false;
        }

        this.polygonEditor.setPolygon(this.currentPolygon);
    }

    addCameraControls() {
        const gfx = this.gfx;
        const canvas = this.canvas;

        canvas.addEventListener('wheel', e => {
            gfx.cameraZoom *= Math.pow(2, SCROLL_SENSITIVITY * -e.deltaY);
        });
    
        let isPanning = false;
    
        canvas.addEventListener('mousedown', e => { if (e.button == 1) isPanning = true });
        document.addEventListener('mouseup', () => isPanning = false);
    
        let prevMousePos = Vec2(0, 0);
    
        document.addEventListener('mousemove', e => {
            const mousePos = Vec2(-e.clientX, e.clientY);
            if (isPanning)
                gfx.cameraPos = gfx.cameraPos.add(mousePos.sub(prevMousePos).muln(2 / gfx.cameraZoom / this.winSize.y * window.devicePixelRatio));
            prevMousePos = mousePos;
            
        });
    }

    drawCheckerboardRect(pos: Vec2, size: Vec2) {
        let mat = Transform.identity();
        mat = mat.translate(pos.add(size.muln(0.5)));
        mat = mat.scale(size);
        this.checkerShader.use();
        this.checkerShader.setColor("u_color1", CHECKERBOARD_COLOR1);
        this.checkerShader.setColor("u_color2", CHECKERBOARD_COLOR2);
        this.checkerShader.setFloat("u_checkerSize", CHECKERBOARD_SIZE);
        this.gfx.setTransformUniform(this.checkerShader, mat);
        this.gfx.quad.drawTriangles();
    }
};