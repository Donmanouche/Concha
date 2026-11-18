// Canvases for Strudel's visuals (@strudel/draw, webaudio's scope / spectrum, .draw() code): they
// record their Canvas 2D calls; Concha's window replays them on images that persist between frames,
// as a browser canvas does (src/plugin/ui/Canvas2D). requestAnimationFrame callbacks run when the
// pattern thread asks for a frame (concha.frame()).
//
// A frame of a canvas: numbers (opcodes and their arguments, a Float64Array) and strings (colours,
// fonts, texts) the opcodes refer to by index. The opcodes are in OPS, mirrored by Canvas2D.h.

export const OPS = {
  fillStyle: 1, strokeStyle: 2, lineWidth: 3, globalAlpha: 4, font: 5, textAlign: 6, textBaseline: 7, lineCap: 8,
  lineJoin: 9, clearRect: 10, fillRect: 11, strokeRect: 12, beginPath: 13, closePath: 14, moveTo: 15, lineTo: 16,
  arc: 17, ellipse: 18, rect: 19, quadraticCurveTo: 20, bezierCurveTo: 21, fill: 22, stroke: 23, fillText: 24,
  strokeText: 25, save: 26, restore: 27, translate: 28, rotate: 29, scale: 30, setTransform: 31, resetTransform: 32,
  snapshot: 33, putImage: 34, arcTo: 35, composite: 36, resize: 37,
};

class Recorder {
  constructor() {
    this.nums = [];
    this.strs = [];
    this.strIndex = new Map();
  }
  str(s) {
    s = String(s);
    let i = this.strIndex.get(s);
    if (i === undefined) {
      i = this.strs.length;
      this.strs.push(s);
      this.strIndex.set(s, i);
    }
    return i;
  }
  op(code, ...args) {
    this.nums.push(code, ...args.map((a) => +a || 0));
  }
  take() {
    const out = { nums: Float64Array.from(this.nums), strs: this.strs };
    this.nums = [];
    this.strs = [];
    this.strIndex = new Map();
    return out;
  }
}

// font size from a CSS font ("bold 12px monospace") for measureText
function fontPx(font) {
  const m = /(\d+(?:\.\d+)?)px/.exec(font);
  return m ? Number(m[1]) : 10;
}

export class Context2D {
  constructor(canvas) {
    this.canvas = canvas;
    this._rec = new Recorder();
    this._state = { fillStyle: '#000000', strokeStyle: '#000000', lineWidth: 1, globalAlpha: 1, font: '10px sans-serif',
      textAlign: 'start', textBaseline: 'alphabetic', lineCap: 'butt', lineJoin: 'miter', globalCompositeOperation: 'source-over' };
    this._stack = [];
    this._snapshots = 0;
  }
  _set(name, code, value, isString) {
    this._state[name] = value;
    isString ? this._rec.op(code, this._rec.str(value)) : this._rec.op(code, value);
  }
  get fillStyle() { return this._state.fillStyle; }
  set fillStyle(v) { this._set('fillStyle', OPS.fillStyle, typeof v === 'string' ? v : '#000000', true); }
  get strokeStyle() { return this._state.strokeStyle; }
  set strokeStyle(v) { this._set('strokeStyle', OPS.strokeStyle, typeof v === 'string' ? v : '#000000', true); }
  get lineWidth() { return this._state.lineWidth; }
  set lineWidth(v) { if (v > 0) this._set('lineWidth', OPS.lineWidth, +v); }
  get globalAlpha() { return this._state.globalAlpha; }
  set globalAlpha(v) { if (v >= 0 && v <= 1) this._set('globalAlpha', OPS.globalAlpha, +v); }
  get font() { return this._state.font; }
  set font(v) { this._set('font', OPS.font, String(v), true); }
  get textAlign() { return this._state.textAlign; }
  set textAlign(v) { this._set('textAlign', OPS.textAlign, String(v), true); }
  get textBaseline() { return this._state.textBaseline; }
  set textBaseline(v) { this._set('textBaseline', OPS.textBaseline, String(v), true); }
  get lineCap() { return this._state.lineCap; }
  set lineCap(v) { this._set('lineCap', OPS.lineCap, String(v), true); }
  get lineJoin() { return this._state.lineJoin; }
  set lineJoin(v) { this._set('lineJoin', OPS.lineJoin, String(v), true); }
  get globalCompositeOperation() { return this._state.globalCompositeOperation; }
  set globalCompositeOperation(v) { this._set('globalCompositeOperation', OPS.composite, String(v), true); }
  clearRect(x, y, w, h) { this._rec.op(OPS.clearRect, x, y, w, h); }
  fillRect(x, y, w, h) { this._rec.op(OPS.fillRect, x, y, w, h); }
  strokeRect(x, y, w, h) { this._rec.op(OPS.strokeRect, x, y, w, h); }
  beginPath() { this._rec.op(OPS.beginPath); }
  closePath() { this._rec.op(OPS.closePath); }
  moveTo(x, y) { this._rec.op(OPS.moveTo, x, y); }
  lineTo(x, y) { this._rec.op(OPS.lineTo, x, y); }
  arc(x, y, r, a0, a1, ccw = false) { this._rec.op(OPS.arc, x, y, Math.max(0, r), a0, a1, ccw ? 1 : 0); }
  ellipse(x, y, rx, ry, rot, a0, a1, ccw = false) { this._rec.op(OPS.ellipse, x, y, Math.max(0, rx), Math.max(0, ry), rot, a0, a1, ccw ? 1 : 0); }
  rect(x, y, w, h) { this._rec.op(OPS.rect, x, y, w, h); }
  arcTo(x1, y1, x2, y2, r) { this._rec.op(OPS.arcTo, x1, y1, x2, y2, r); }
  quadraticCurveTo(cx, cy, x, y) { this._rec.op(OPS.quadraticCurveTo, cx, cy, x, y); }
  bezierCurveTo(c1x, c1y, c2x, c2y, x, y) { this._rec.op(OPS.bezierCurveTo, c1x, c1y, c2x, c2y, x, y); }
  fill() { this._rec.op(OPS.fill); }
  stroke() { this._rec.op(OPS.stroke); }
  fillText(t, x, y) { this._rec.op(OPS.fillText, this._rec.str(t), x, y); }
  strokeText(t, x, y) { this._rec.op(OPS.strokeText, this._rec.str(t), x, y); }
  measureText(t) {
    const px = fontPx(this._state.font);
    return { width: String(t).length * px * 0.6, actualBoundingBoxAscent: px * 0.8, actualBoundingBoxDescent: px * 0.2 };
  }
  save() { this._stack.push({ ...this._state }); this._rec.op(OPS.save); }
  restore() { if (this._stack.length) this._state = this._stack.pop(); this._rec.op(OPS.restore); }
  translate(x, y) { this._rec.op(OPS.translate, x, y); }
  rotate(a) { this._rec.op(OPS.rotate, a); }
  scale(x, y) { this._rec.op(OPS.scale, x, y); }
  setTransform(a, b, c, d, e, f) {
    if (typeof a === 'object' && a) ({ a, b, c, d, e, f } = a);
    this._rec.op(OPS.setTransform, a, b, c, d, e, f);
  }
  resetTransform() { this._rec.op(OPS.resetTransform); }
  // pixels stay in the window: an image is a reference to a copy made there
  getImageData(x, y, w, h) {
    const k = ++this._snapshots;
    this._rec.op(OPS.snapshot, k, x, y, w, h);
    return { __snapshot: k, width: w, height: h, data: new Uint8ClampedArray(0) };
  }
  putImageData(image, dx, dy) {
    if (image?.__snapshot) this._rec.op(OPS.putImage, image.__snapshot, dx, dy);
  }
  createImageData(w, h) { return { width: w, height: h, data: new Uint8ClampedArray(0) }; }
  drawImage() {}
  setLineDash() {}
  getLineDash() { return []; }
  createLinearGradient() { return { addColorStop() {} }; }
  createRadialGradient() { return { addColorStop() {} }; }
}

let nextCanvasId = 1;
const canvases = new Map(); // id -> Canvas
export let viewport = { width: 800, height: 600, pixelRatio: 1 };

export class Canvas {
  constructor(id) {
    this.id = id || `canvas-${nextCanvasId++}`;
    this.style = {};
    this._width = viewport.width;
    this._height = viewport.height;
    this._ctx = null;
    this.widget = false; // an inline widget (._pianoroll()...), drawn next to its code
    canvases.set(this.id, this);
  }
  get width() { return this._width; }
  set width(v) { this._resize(v, this._height); }
  get height() { return this._height; }
  set height(v) { this._resize(this._width, v); }
  _resize(w, h) {
    w = Math.max(1, Math.round(+w || 1));
    h = Math.max(1, Math.round(+h || 1));
    if (w === this._width && h === this._height) return;
    this._width = w;
    this._height = h;
    this._ctx?._rec.op(OPS.resize, w, h);
  }
  getContext() {
    if (!this._ctx) {
      this._ctx = new Context2D(this);
      this._ctx._rec.op(OPS.resize, this._width, this._height);
    }
    return this._ctx;
  }
  getBoundingClientRect() { return { x: 0, y: 0, left: 0, top: 0, width: this._width, height: this._height }; }
  addEventListener() {}
  removeEventListener() {}
  setAttribute() {}
  remove() { canvases.delete(this.id); }
}

export const getCanvas = (id) => canvases.get(id);

// The window's size: the background canvases follow it (as draw.mjs's resize listener).
export function setViewport(width, height, pixelRatio = 1) {
  viewport = { width, height, pixelRatio };
  for (const c of canvases.values()) if (!c.widget) c._resize(width * pixelRatio, height * pixelRatio);
}

// ---- requestAnimationFrame: run by frame() ----
let rafId = 0;
let callbacks = new Map();
export function requestAnimationFrame(fn) {
  const id = ++rafId;
  callbacks.set(id, fn);
  return id;
}
export function cancelAnimationFrame(id) {
  callbacks.delete(id);
}
export const hasFrames = () => callbacks.size > 0;

// One frame: the pending callbacks (each asks for the next frame itself), then what every canvas
// recorded since the last frame.
export function runFrame(timeMs) {
  const pending = callbacks;
  callbacks = new Map();
  for (const fn of pending.values()) {
    try {
      fn(timeMs);
    } catch (e) {
      globalThis.__conchaLog?.push({ message: `[draw] ${e?.message ?? e}`, type: 'error' });
    }
  }
  const out = [];
  for (const c of canvases.values()) {
    if (!c._ctx) continue;
    const { nums, strs } = c._ctx._rec.take();
    if (!nums.length && !c._announced) continue;
    c._announced = true;
    out.push({ id: c.id, widget: c.widget, width: c._width, height: c._height, nums, strs });
  }
  return out;
}

// A new evaluation: the old code's animations stop and its canvases go (draw.mjs cleanupDraw).
export function resetCanvases() {
  callbacks = new Map();
  canvases.clear();
}

// ---- the browser globals draw.mjs and user code use ----
{
  const g = globalThis;
  const doc = g.document;
  const elementById = (id) => getCanvas(String(id).replace(/^#/, '')) ?? null;
  doc.getElementById = elementById;
  doc.querySelector = (sel) => (typeof sel === 'string' && sel.startsWith('#') ? elementById(sel) : null);
  const createElement = doc.createElement;
  doc.createElement = (tag) => (String(tag).toLowerCase() === 'canvas' ? new Canvas() : createElement(tag));
  doc.body.prepend = () => {};
  doc.body.append = () => {};
  doc.documentElement ??= { style: {} };
  g.getComputedStyle ??= () => ({ getPropertyValue: () => '' });
  Object.defineProperty(g.window, 'innerWidth', { get: () => viewport.width, configurable: true });
  Object.defineProperty(g.window, 'innerHeight', { get: () => viewport.height, configurable: true });
  Object.defineProperty(g.window, 'devicePixelRatio', { get: () => viewport.pixelRatio, configurable: true });
  g.requestAnimationFrame = requestAnimationFrame;
  g.cancelAnimationFrame = cancelAnimationFrame;
}
