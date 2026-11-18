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
if (!g.document) {
  g.document = {
    addEventListener: noop,
    removeEventListener: noop,
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
if (!g.navigator) g.navigator = { userAgent: 'Concha', getGamepads: () => [] };
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
