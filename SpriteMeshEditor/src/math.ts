class typeVec2 {
    readonly x: number;
    readonly y: number;

    constructor(x: number, y: number) {
        this.x = x;
        this.y = y;
    }

    add(v: typeVec2): typeVec2 { return new typeVec2(this.x + v.x, this.y + v.y); }
    addn(v: number): typeVec2 { return this.add(new typeVec2(v, v)); }

    sub(v: typeVec2): typeVec2 { return new typeVec2(this.x - v.x, this.y - v.y); }
    subn(v: number): typeVec2 { return this.sub(new typeVec2(v, v)); }

    mul(v: typeVec2): typeVec2 { return new typeVec2(this.x * v.x, this.y * v.y); }
    muln(v: number): typeVec2 { return this.mul(new typeVec2(v, v)); }

    div(v: typeVec2): typeVec2 { return new typeVec2(this.x / v.x, this.y / v.y); }
    divn(v: number): typeVec2 { return this.div(new typeVec2(v, v)); }

    neg(): typeVec2 { return new typeVec2(-this.x, -this.y); }

    dot(v: typeVec2): number {
        return this.x * v.x + this.y * v.y;
    }

    get lengthSquared(): number { return this.x * this.x + this.y * this.y; }
    get length(): number { return Math.sqrt(this.lengthSquared); }

    normalize(): typeVec2 {
        return this.divn(this.length);
    }

    getCW(): typeVec2 {
        return new typeVec2(this.y, -this.x);
    }

    getCCW(): typeVec2 {
        return new typeVec2(-this.y, this.x);
    }

    rotateRaw(cos: number, sin: number): typeVec2 {
        return new typeVec2(
            this.x * cos + this.y * sin,
            this.x * sin - this.y * cos
        );
    }

    rotate(angle: number): typeVec2 {
        return this.rotateRaw(Math.cos(angle), Math.sin(angle));
    }

    toString(): string {
        return `[${this.x}, ${this.y}]`;
    }
};

export type Vec2 = typeVec2;
export function Vec2(x: number, y: number): Vec2 {
    return new typeVec2(x, y);
}

export class Transform {
    readonly unitX: Vec2;
    readonly unitY: Vec2;
    readonly offset: Vec2;

    constructor(unitX: Vec2, unitY: Vec2, offset: Vec2) {
        this.unitX  = unitX;
        this.unitY  = unitY;
        this.offset = offset;
    }

    toString(): string {
        return `[${this.unitX.x}, ${this.unitX.y}, ${this.offset.x}]\n[${this.unitY.x}, ${this.unitY.y}, ${this.offset.y}]`;
    }

    get a(): number { return this.unitX.x; }
    get b(): number { return this.unitX.y; }
    get c(): number { return this.unitY.x; }
    get d(): number { return this.unitY.y; }
    get tx(): number { return this.offset.x; }
    get ty(): number { return this.offset.y; }

    static identity() {
        return new Transform(Vec2(1, 0), Vec2(0, 1), Vec2(0, 0));
    }

    transformVector(vector: Vec2): Vec2 {
        return this.unitX.muln(vector.x).add(this.unitY.muln(vector.y));
    }

    transformPoint(point: Vec2): Vec2 {
        return this.transformVector(point).add(this.offset);
    }

    mul(t2: Transform): Transform {
        const t1 = this;
        return new Transform(
            Vec2(t1.a * t2.a + t1.b * t2.c, t1.a * t2.b + t1.b * t2.d),
            Vec2(t1.c * t2.a + t1.d * t2.c, t1.c * t2.b + t1.d * t2.d),
            t2.transformVector(t1.offset).add(t2.offset)
        );
    }

    translate(offset: Vec2) {
        return new Transform(this.unitX, this.unitY, this.offset.add(this.transformVector(offset)));
    }

    scale(scale: Vec2) {
        return new Transform(this.unitX.muln(scale.x), this.unitY.muln(scale.y), this.offset);
    }

    rotate(angle: number) {
        const cos = Math.cos(angle);
        const sin = Math.sin(angle);
        return new Transform(
            this.unitX.rotateRaw(cos, sin),
            this.unitY.rotateRaw(cos, sin),
            this.offset
        );
    }

    data(): number[] {
        return [
            this.unitX.x,  this.unitX.y,  0,
            this.unitY.x,  this.unitY.y,  0,
            this.offset.x, this.offset.y, 1
        ];
    }

    invert(): Transform {
        const determinant = 1 / (this.a * this.d - this.b * this.c);
        return new Transform(
            Vec2( determinant * this.d, -determinant * this.b),
            Vec2(-determinant * this.c,  determinant * this.a),
            Vec2( determinant * (this.c * this.ty - this.d * this.tx), determinant * (this.b * this.tx - this.a * this.ty))
        );
    }
};