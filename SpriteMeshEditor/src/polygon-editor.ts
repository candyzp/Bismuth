import type { Editor } from "./editor";
import { Color } from "./graphics";
import { Vec2 } from "./math";

export type Polygon = Vec2[];

const POLYGON_PATH_COLOR       = Color(0, 0, 0);
const POLYGON_PATH_HOVER_COLOR = Color(1, 0, 0);
const POLYGON_NEW_POINT_COLOR  = Color(0, 0, 0, 0.5);

const POINT_ALIGN_THRESHOLD = 10;

const LINE_HOVER_DISTANCE = 30;

type HoverState = null |
                  { type: 'point', index: number } |
                  { type: 'line',  index: number, point: Vec2 };
type EditState  = null | { type: 'point', index: number, offset: Vec2 };

function clamp(a: number, min: number, max: number) {
    return Math.max(min, Math.min(a, max));
}

export class PolygonEditor {
    polygon: Polygon = [];

    editor: Editor;

    hover: HoverState = null;
    edit: EditState   = null;
    
    constructor(editor: Editor) {
        this.editor = editor;
    }

    setPolygon(poly: Polygon) {
        this.polygon = poly;
    }

    render() {
        if (this.polygon.length == 0)
            return;

        const editor = this.editor;

        const cpolygon: Vec2[] = [];
        for (const p of this.polygon)
            cpolygon.push(p);
        cpolygon.push(this.polygon[0]);
        editor.gfx.lineWidth = 30;

        if (this.hover && this.hover.type == 'line') {
            const index = this.hover.index;
            editor.gfx.drawPath(cpolygon.slice(0, index + 1), POLYGON_PATH_COLOR);
            editor.gfx.drawPath(cpolygon.slice(index, index + 2), POLYGON_PATH_HOVER_COLOR);
            editor.gfx.drawPath(cpolygon.slice(index + 1), POLYGON_PATH_COLOR);
            editor.drawPoint(this.hover.point, POLYGON_NEW_POINT_COLOR);
        } else
            editor.gfx.drawPath(cpolygon, POLYGON_PATH_COLOR);
        
        for (let i = 0; i < this.polygon.length; i++) {
            const point = this.polygon[i];
            let color = POLYGON_PATH_COLOR;

            if (this.hover && this.hover.type == 'point' && this.hover.index == i)
                color = POLYGON_PATH_HOVER_COLOR;

            editor.drawPoint(point, color);
        }
    }

    onMouseInput(button: number, pos: Vec2, pressed: boolean) {
        if (!pressed) {
            this.edit = null;
            return;
        }

        if (!this.hover) return;

        if (this.hover.type == 'point') {
            if (button == 0) {
                this.edit = {
                    type: 'point',
                    index: this.hover.index,
                    offset: this.polygon[this.hover.index].sub(pos)
                }
            } else if (button == 2) {
                this.polygon.splice(this.hover.index, 1);
                this.edit = null;
                this.hover = null;
                this.editor.polygonShouldBeSaved = true;
            }
        } else if (this.hover.type == 'line' && button == 0) {
            const newIndex = this.hover.index + 1;
            this.polygon.splice(newIndex, 0, this.hover.point);
            this.edit = { type: 'point', index: newIndex, offset: this.hover.point.sub(pos) };
            this.hover = { type: 'point', index: newIndex };
            this.editor.polygonShouldBeSaved = true;
        }
    }

    getNextIndex(index: number): number {
        if ((index + 1) >= this.polygon.length)
            return 0;
        return index + 1;
    }

    getPrevIndex(index: number): number {
        if (index <= 0)
            return this.polygon.length - 1;
        return index - 1;
    }

    alignPoint(point: Vec2, ref: Vec2): Vec2 {
        const thres = POINT_ALIGN_THRESHOLD / this.editor.gfx.cameraZoom / this.editor.winSize.y;
        const d = point.sub(ref);
        if (Math.abs(d.x) < thres)
            return Vec2(ref.x, point.y);
        if (Math.abs(d.y) < thres)
            return Vec2(point.x, ref.y);
        return point;
    }

    isHoveringOverLine(pos: Vec2, index: number): [boolean, Vec2] {
        const dist = LINE_HOVER_DISTANCE / this.editor.gfx.cameraZoom / this.editor.winSize.y;

        const p1 = this.polygon[index];
        const p2 = this.polygon[this.getNextIndex(index)];

        if (Math.abs(p2.sub(p1).getCW().normalize().dot(pos.sub(p1))) >= dist)
            return [false, Vec2(0, 0)];

        const ldist = p2.sub(p1).normalize().dot(pos.sub(p1));

        if (ldist < 0 || ldist > p2.sub(p1).length)
            return [false, Vec2(0, 0)];

        return [true, p2.sub(p1).normalize().muln(ldist).add(p1)];
    }

    mouseMouseWhileEditing(pos: Vec2) {
        if (!this.edit) return;
        const edit = this.edit;

        if (edit.type == 'point') {
            let point = pos.add(edit.offset);

            const nindex = this.getNextIndex(edit.index);
            const pindex = this.getPrevIndex(edit.index);
            if (this.editor.shouldAlignPoints.value && nindex != edit.index && pindex != edit.index) {
                point = this.alignPoint(point, this.polygon[nindex]);
                point = this.alignPoint(point, this.polygon[pindex]);
            }

            const gridAlign = this.editor.pointGridAlignment.value;
            if (gridAlign != 'none' && typeof gridAlign != 'string') {
                point = Vec2(
                    Math.round(point.x / gridAlign) * gridAlign,
                    Math.round(point.y / gridAlign) * gridAlign
                );
            }

            point = Vec2(
                clamp(point.x, 0, this.editor.currentSpriteSize.x),
                clamp(point.y, 0, this.editor.currentSpriteSize.y)
            );

            this.polygon[edit.index] = point;
            this.editor.polygonShouldBeSaved = true;
        }
    }

    onMouseMove(pos: Vec2) {
        if (this.edit) {
            this.mouseMouseWhileEditing(pos);
            return;
        }

        this.hover = null;

        for (let index = 0; index < this.polygon.length; index++) {
            if (this.editor.isHoveringOverPoint(this.polygon[index], pos)) {
                this.hover = { type: 'point', index };
                return;
            }
        }

        for (let index = 0; index < this.polygon.length; index++) {
            const [cond, point] = this.isHoveringOverLine(pos, index);
            if (cond) {
                this.hover = { type: 'line', index, point };
                return;
            }
        }
    }
};
