// Browser globals that Strudel touches, for running it in QuickJS (and in node for the tests).
// Log events that Strudel dispatches on `document` are collected in globalThis.__conchaLog.
const g = globalThis;
const noop = () => {};
g.__conchaLog ??= [];

if (!g.performance) {
  const t0 = Date.now();
  g.performance = { now: () => Date.now() - t0 };
}
if (!g.CustomEvent) {
  g.CustomEvent = class CustomEvent {
    constructor(type, init) {
      this.type = type;
      this.detail = init?.detail;
    }
  };
}
// document listeners: only `mousemove` is ever fired (by __conchaMouse, from the window's mouse)
const listeners = new Map();
g.__conchaMouse = () => {
  const m = typeof g.__concha_mouse === 'function' ? g.__concha_mouse() : null;
  const fns = listeners.get('mousemove');
  if (!m || !fns || (m[0] === g.__conchaMouseLast?.[0] && m[1] === g.__conchaMouseLast?.[1])) return;
  g.__conchaMouseLast = m;
  for (const fn of fns) fn({ type: 'mousemove', clientX: m[0], clientY: m[1] }); // body is 1 x 1
};
if (!g.document) {
  g.document = {
    addEventListener: (type, fn) => {
      if (typeof fn !== 'function') return;
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(fn);
    },
    removeEventListener: (type, fn) => listeners.get(type)?.delete(fn),
    dispatchEvent: (e) => {
      if (e?.type === 'strudel.log') g.__conchaLog.push({ message: String(e.detail?.message), type: e.detail?.type ?? 'info' });
      return true;
    },
    getElementById: () => null,
    querySelector: () => null,
    createElement: () => ({ style: {}, getContext: () => null, appendChild: noop, setAttribute: noop }),
    body: { clientWidth: 1, clientHeight: 1, appendChild: noop },
  };
}
if (!g.window) g.window = g;
if (!g.window.addEventListener) {
  g.window.addEventListener = noop;
  g.window.removeEventListener = noop;
}
g.window.devicePixelRatio ??= 1;
// Gamepads (@strudel/gamepad): the plug-in's reader (src/input/Gamepad) through __concha_gamepad(i),
// read only for the index asked
const gamepads = new Proxy([], {
  get(target, prop) {
    if (typeof prop === 'string' && /^[0-3]$/.test(prop)) {
      const s = typeof g.__concha_gamepad === 'function' ? g.__concha_gamepad(Number(prop)) : null;
      return s ? { index: Number(prop), connected: true, axes: s.axes, buttons: s.buttons.map((v) => ({ value: v, pressed: v > 0.5 })) } : null;
    }
    if (prop === 'length') return 4;
    return target[prop];
  },
});
if (!g.navigator) g.navigator = { userAgent: 'Concha', getGamepads: () => gamepads };
if (!g.location) g.location = { href: 'concha://', origin: 'concha://', search: '', hash: '' };
// Timers: Concha drives everything from C++, nothing waits on a timer.
if (!g.setTimeout) {
  g.setTimeout = () => 0;
  g.clearTimeout = noop;
}
if (!g.setInterval) {
  g.setInterval = () => 0;
  g.clearInterval = noop;
}
