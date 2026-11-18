// Strudel's visuals in Concha: @strudel/draw (pianoroll, punchcard, wordfall, spiral, pitchwheel,
// animate, .draw()), webaudio's scope / fscope / spectrum, and the inline widgets of strudel.cc's
// editor (codemirror/widget.mjs: ._pianoroll(), ._punchcard(), ._spiral(), ._scope(),
// ._pitchwheel(), ._spectrum()). They draw on js/src/canvas.mjs's canvases, unchanged.
import { Pattern, registerControl } from '@strudel/core';
import { registerWidgetType, registerLanguage } from '@strudel/transpiler';
import * as draw from '@strudel/draw';
import '@strudel/webaudio/scope.mjs';
import '@strudel/webaudio/spectrum.mjs';
import { Canvas, getCanvas, viewport } from './canvas.mjs';

export { draw };

// Concha's shaders (not Strudel's): shader(`...`) sets a GLSL fragment shader drawn behind the code,
// Shadertoy style (mainImage(out vec4 fragColor, in vec2 fragCoord); iResolution, iTime, iCycle,
// iCps, iChannel0 = the output's spectrum (row 0) and waveform (row 1)). `.uniform("name")` on a
// pattern feeds `uniform float name;` (seconds since its last onset) and `name_v` (its gain).
let shaderSource;
export function shader(code) {
  shaderSource = code == null ? undefined : String(code);
}
export const takeShader = () => shaderSource;
export const resetShader = () => {
  shaderSource = undefined;
};
export const { uniform } = registerControl('uniform');
// Backtick strings are mini-notation to Strudel's transpiler: shader`...` and glsl`...` are language
// literals instead (as mondo`...`), whose text arrives untouched: shader`...` sets the shader,
// glsl`...` is the text (shader(glsl`...`)).
export const glsl = (code) => String(code);
registerLanguage('shader', { getLocations: () => [] });
registerLanguage('glsl', { getLocations: () => [] });

// strudel.cc's default theme (codemirror/themes/strudel-theme.mjs settings)
draw.setTheme({
  background: '#222',
  lineBackground: '#22222299',
  foreground: '#fff',
  muted: '#8a919966',
  caret: '#ffcc00',
  selection: 'rgba(128, 203, 196, 0.5)',
  selectionMatch: '#036dd626',
  lineHighlight: '#00000050',
  gutterBackground: 'transparent',
  gutterForeground: '#8a919966',
});

function getCanvasWidget(id, options = {}) {
  const { width = 500, height = 60, pixelRatio = viewport.pixelRatio } = options;
  const canvas = getCanvas(id) ?? new Canvas(id);
  canvas.widget = true;
  canvas.width = width * pixelRatio;
  canvas.height = height * pixelRatio;
  return canvas;
}

function registerWidget(type, fn) {
  registerWidgetType(type);
  Pattern.prototype[type] = function (id, options = { fold: 1 }) {
    return fn(id, options, this);
  };
}

registerWidget('_pianoroll', (id, options = {}, pat) => {
  const ctx = getCanvasWidget(id, options).getContext('2d');
  return pat.tag(id).pianoroll({ fold: 1, ...options, ctx, id });
});
registerWidget('_punchcard', (id, options = {}, pat) => {
  const ctx = getCanvasWidget(id, options).getContext('2d');
  return pat.tag(id).punchcard({ fold: 1, ...options, ctx, id });
});
registerWidget('_spiral', (id, options = {}, pat) => {
  let _size = options.size || 275;
  options = { width: _size, height: _size, ...options, size: _size / 5 };
  const ctx = getCanvasWidget(id, options).getContext('2d');
  return pat.tag(id).spiral({ ...options, ctx, id });
});
registerWidget('_scope', (id, options = {}, pat) => {
  options = { width: 500, height: 60, pos: 0.5, scale: 1, ...options };
  const ctx = getCanvasWidget(id, options).getContext('2d');
  return pat.tag(id).scope({ ...options, ctx, id });
});
registerWidget('_pitchwheel', (id, options = {}, pat) => {
  let _size = options.size || 200;
  options = { width: _size, height: _size, ...options, size: _size / 5 };
  const ctx = getCanvasWidget(id, options).getContext('2d');
  return pat.pitchwheel({ ...options, ctx, id });
});
registerWidget('_spectrum', (id, options = {}, pat) => {
  let _size = options.size || 200;
  options = { width: _size, height: _size, ...options, size: _size / 5 };
  const ctx = getCanvasWidget(id, options).getContext('2d');
  return pat.spectrum({ ...options, ctx, id });
});
