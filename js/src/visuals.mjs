// Strudel's visuals in Concha, drawn by the window as a laboratory oscilloscope (plugin/ui/ScopeStrip):
//  - .pianoroll(), .punchcard(), .wordfall(), .spiral(), .pitchwheel() and their inline forms
//    ._pianoroll()... mark the haps of the code they are called on (context.conchaVisual): each `$:`
//    block with marked haps becomes a channel of the main screen;
//  - .scope() / .tscope() / ._scope() show the SCOPE screen, .spectrum() / .fscope() / ._spectrum()
//    the FFT screen, of the analyser they set (.analyze(id), as Strudel);
//  - nothing is drawn in JS for these. A user's own .draw() / .onPaint() / animate() still runs
//    Strudel's draw.mjs on js/src/canvas.mjs's canvases (customDraw), as before.
import { Pattern, registerControl } from '@strudel/core';
import { registerWidgetType, registerLanguage } from '@strudel/transpiler';
import * as draw from '@strudel/draw';

export { draw };

// what the evaluated code asked for (reset before each evaluation)
const requested = { channels: false, scope: new Set(), fft: new Set(), custom: false };
export function resetVisuals() {
  requested.channels = false;
  requested.scope = new Set();
  requested.fft = new Set();
  requested.custom = false;
}
export const takeVisuals = () => ({
  channels: requested.channels,
  scope: [...requested.scope],
  fft: [...requested.fft],
  custom: requested.custom,
});

const channel = (kind) =>
  function () {
    requested.channels = true;
    return this.withContext((c) => ({ ...c, conchaVisual: kind }));
  };
for (const kind of ['pianoroll', 'punchcard', 'wordfall', 'spiral', 'pitchwheel']) {
  Pattern.prototype[kind] = channel(kind);
  registerWidgetType('_' + kind);
  Pattern.prototype['_' + kind] = function () {
    return this[kind]();
  };
}
function scope(config = {}) {
  const id = config?.id ?? 1;
  requested.scope.add(String(id));
  return this.analyze(id);
}
function fft(config = {}) {
  const id = config?.id ?? 1;
  requested.fft.add(String(id));
  return this.analyze(id);
}
Pattern.prototype.scope = Pattern.prototype.tscope = scope;
Pattern.prototype.fscope = Pattern.prototype.spectrum = fft;
registerWidgetType('_scope');
registerWidgetType('_spectrum');
Pattern.prototype._scope = function (id, options = {}) {
  return this.scope({ ...options, id: options.id ?? id });
};
Pattern.prototype._spectrum = function (id, options = {}) {
  return this.spectrum({ ...options, id: options.id ?? id });
};
// the user's own drawings keep Strudel's canvas path
for (const name of ['draw', 'onPaint', 'animate']) {
  const original = Pattern.prototype[name];
  if (typeof original === 'function')
    Pattern.prototype[name] = function (...args) {
      requested.custom = true;
      return original.apply(this, args);
    };
}

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
