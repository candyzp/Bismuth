import { isClockwise, triangulate } from "poly-partition";
import { parse } from "./fast-plist";
import { VertexBuffer, type Graphics } from "./graphics";
import { Transform, Vec2 } from "./math";
import { ensureCounterClockwise } from "./editor";

type SpritesheetPList = {
    frames: {
        [_: string]: {
            aliases: string[],
            spriteOffset: string,
            spriteSourceSize: string,
            textureRect: string,
            textureRotated: boolean
        }
    }
};

export type Rect = { x: number, y: number, w: number, h: number };
export function Rect(x: number, y: number, w: number, h: number): Rect { return {x, y, w, h}; }

interface SpriteFrame {
    offset: Vec2,
    size: Vec2,
    rect: Rect,
    rotated: boolean
};

interface SpriteVertexTransforms {
    vertexPositionTransform: Transform;
    textureCoordTransform: Transform;
};

export class Spritesheet {
    gfx: Graphics;
    image: HTMLImageElement;
    plist: SpritesheetPList;
    frameNames: string[];
    texture: WebGLTexture;
    vertexBuffers: { [_: string]: VertexBuffer } = {};
    vertexTransforms: { [_: string]: SpriteVertexTransforms } = {};

    constructor(gfx: Graphics, plist: string, image: HTMLImageElement) {
        this.gfx = gfx;
        this.image = image;
        this.plist = parse(plist);
        this.frameNames = Object.keys(this.plist.frames);
        this.texture = gfx.createTexture(image);
    }

    getFrame(name: string): SpriteFrame {
        if (!this.plist.frames[name])
            throw "Sprite frame with name " + name + " not found";
        const frame = this.plist.frames[name];
        const offset = Spritesheet.parseArrayString(frame.spriteOffset) as [number, number];
        const size   = Spritesheet.parseArrayString(frame.spriteSourceSize) as [number, number];
        const rect   = Spritesheet.parseArrayString(frame.textureRect) as [number, number, number, number];
        return {
            offset: Vec2(...offset),
            size: Vec2(...size),
            rect: Rect(...rect),
            rotated: frame.textureRotated
        };
    }

    getSpriteVertexTransforms(name: string): SpriteVertexTransforms {
        if (this.vertexTransforms[name])
            return this.vertexTransforms[name];
        const frame = this.getFrame(name);

        //// CALCULATING POSITION TRANSFORM ////

        const posBottomLeft  = Vec2(0, 0);
        const posRightVector = Vec2(frame.rect.w, 0);
        const posUpVector    = Vec2(0, frame.rect.h);

        const posTransform = new Transform(posRightVector, posUpVector, posBottomLeft);

        //// CALCULATING TEXTURE COORDINATE TRANSFORM ////

        let texWidth  = frame.rect.w / this.image.width;
        let texHeight = frame.rect.h / this.image.height;
        if (frame.rotated) {
            const tmp = texWidth;
            texWidth = texHeight;
            texHeight = tmp;
        }

        let texLeft   = frame.rect.x / this.image.width;
        let texBottom = frame.rect.y / this.image.height + texHeight;

        let texBottomLeft: Vec2;
        let texRightVector: Vec2;
        let texUpVector: Vec2;

        if (!frame.rotated) {
            texBottomLeft  = Vec2(texLeft, texBottom);
            texRightVector = Vec2(texWidth, 0);
            texUpVector    = Vec2(0, -texHeight);
        } else {
            texBottomLeft  = Vec2(texLeft, texBottom - texHeight);
            texRightVector = Vec2(0, texHeight);
            texUpVector    = Vec2(texWidth, 0);
        }

        const texTransform = new Transform(texRightVector, texUpVector, texBottomLeft);
        
        const transform: SpriteVertexTransforms = {
            vertexPositionTransform: posTransform,
            textureCoordTransform:   texTransform
        };

        this.vertexTransforms[name] = transform;
        return transform;
    }

    createSpriteVertexBuffer(name: string): VertexBuffer {
        if (this.vertexBuffers[name])
            return this.vertexBuffers[name];

        const transforms = this.getSpriteVertexTransforms(name);
        const posTrans = transforms.vertexPositionTransform;
        const texTrans = transforms.textureCoordTransform;

        const bottomLeft  = posTrans.transformPoint(Vec2(0, 0));
        const bottomRight = posTrans.transformPoint(Vec2(1, 0));
        const topLeft     = posTrans.transformPoint(Vec2(0, 1));
        const topRight    = posTrans.transformPoint(Vec2(1, 1));

        const texBottomLeft  = texTrans.transformPoint(Vec2(0, 0));
        const texBottomRight = texTrans.transformPoint(Vec2(1, 0));
        const texTopLeft     = texTrans.transformPoint(Vec2(0, 1));
        const texTopRight    = texTrans.transformPoint(Vec2(1, 1));

        const vbuf = new VertexBuffer(this.gfx);

        vbuf.posTex(bottomLeft,  texBottomLeft);
        vbuf.posTex(topLeft,     texTopLeft);
        vbuf.posTex(topRight,    texTopRight);
        vbuf.posTex(bottomLeft,  texBottomLeft);
        vbuf.posTex(topRight,    texTopRight);
        vbuf.posTex(bottomRight, texBottomRight);

        this.vertexBuffers[name] = vbuf;
        return vbuf;
    }

    createMeshSpriteVertexBuffer(name: string, polygon: Vec2[]): VertexBuffer {
        const frame = this.getFrame(name);

        const vbuffer = new VertexBuffer(this.gfx);
        const transforms = this.getSpriteVertexTransforms(name);

        const transformAndAddPoint = (point: { x: number, y: number }) => {
            const p = Vec2(point.x, point.y).div(Vec2(frame.rect.w, frame.rect.h));

            vbuffer.posTex(
                transforms.vertexPositionTransform.transformPoint(p),
                transforms.textureCoordTransform.transformPoint(p)
            );
        }

        polygon = ensureCounterClockwise(polygon);

        const triangles = triangulate(polygon);
        for (const triangle of triangles) {
            transformAndAddPoint(triangle[0]);
            transformAndAddPoint(triangle[1]);
            transformAndAddPoint(triangle[2]);
        }

        return vbuffer;
    }

    static parseArrayString(str: string): number[] {
        const ret: number[] = [];
        for (const s of str.split(',')) {
            let ns = '';
            for (const c of s)
                if (!['{', '}'].includes(c))
                    ns += c;
            ret.push(+ns);
        }
        return ret;
    }

    static async load(gfx: Graphics, path: string): Promise<Spritesheet> {
        const plist = await (await fetch(path + '.plist')).text();
        const image = new Image();
        image.src = path + '.png';
        await new Promise((resolve, reject) => {
            image.onload = resolve;
            image.onerror = reject;
        });

        return new Spritesheet(gfx, plist, image);
    }
};