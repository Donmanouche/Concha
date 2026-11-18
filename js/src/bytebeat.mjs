// bytebeat for Concha: the expression helpers of superdough's byte-beat-processor
// (packages/superdough/worklets.mjs and synth.mjs, AGPL-3.0, Strudel contributors), used to render
// a bytebeat note into a buffer on the pattern thread (the C++ engine cannot run JavaScript).
const PI = Math.PI;
const clamp = (num, min, max) => Math.min(Math.max(num, min), max);

/**  BYTE BEATS
 * @tags internals
 */
const chyx = {
  /*bit*/ bitC: function (x, y, z) {
    return x & y ? z : 0;
  },
  /*bit reverse*/ br: function (x, size = 8) {
    if (size > 32) {
      throw new Error('br() Size cannot be greater than 32');
    }
    let result = 0;
    for (let idx = 0; idx < size; idx++) {
      result |= chyx.bitC(x, 1 << idx, 1 << (size - (idx + 1)));
    }
    return result;
  },
  /*sin that loops every 128 "steps", instead of every pi steps*/ sinf: function (x) {
    return Math.sin((x * PI) / 128);
  },
  /*cos that loops every 128 "steps", instead of every pi steps*/ cosf: function (x) {
    return Math.cos((x * PI) / 128);
  },
  /*tan that loops every 128 "steps", instead of every pi steps*/ tanf: function (x) {
    return Math.tan((x * PI) / 128);
  },
  /*converts t into a string composed of its bits; regexes that*/ regG: function (t, X) {
    return X.test(t.toString(2));
  },
};

// Create shortened Math functions
let mathParams, byteBeatHelperFuncs;
function getByteBeatFunc(codetext) {
  if (mathParams == null) {
    mathParams = Object.getOwnPropertyNames(Math);
    byteBeatHelperFuncs = mathParams.map((k) => Math[k]);
    const chyxNames = Object.getOwnPropertyNames(chyx);
    const chyxFuncs = chyxNames.map((k) => chyx[k]);
    mathParams.push('int', 'window', ...chyxNames);
    byteBeatHelperFuncs.push(Math.floor, globalThis, ...chyxFuncs);
  }
  return new Function(...mathParams, 't', `return 0,\n${codetext || 0};`).bind(globalThis, ...byteBeatHelperFuncs);
}


export const defaultBeats = [
    '(t%255 >= t/255%255)*255',
    '(t*(t*8%60 <= 300)|(-t)*(t*4%512 < 256))+t/400',
    't',
    't*(t >> 10^t)',
    't&128',
    't&t>>8',
    '((t%255+t%128+t%64+t%32+t%16+t%127.8+t%64.8+t%32.8+t%16.8)/3)',
    '((t%64+t%63.8+t%64.15+t%64.35+t%63.5)/1.25)',
    '(t&(t>>7)-t)',
    '(sin(t*PI/128)*127+127)',
    '((t^t/2+t+64*(sin((t*PI/64)+(t*PI/32768))+64))%128*2)',
    '((t^t/2+t+64*(cos >> 0))%127.85*2)',
    '((t^t/2+t+64)%128*2)',
    '(((t * .25)^(t * .25)/100+(t * .25))%128)*2',
    '((t^t/2+t+64)%7 * 24)',
  ];

// byte-beat-processor's loop: t starts at begin * sampleRate (or 0 with byteBeatStartTime, then
// offset by it) on the first processed sample; the C++ engine plays the buffer from there.
export function renderByteBeat(codeText, frequency, begin, frames, sampleRate, byteBeatStartTime) {
  codeText = String(codeText)
    .trim()
    .replace(
      /^eval\(unescape\(escape(?:`|\('|\("|\(`)(.*?)(?:`|'\)|"\)|`\)).replace\(\/u\(\.\.\)\/g,["'`]\$1%["'`]\)\)\)$/,
      (match, m1) => unescape(escape(m1).replace(/u(..)/g, '$1%')),
    );
  const func = getByteBeatFunc(codeText);
  let t = begin * sampleRate;
  let initialOffset = 0;
  if (byteBeatStartTime != null) {
    t = 0;
    initialOffset = Math.floor(byteBeatStartTime);
  }
  const scale = 256 / sampleRate;
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    const funcValue = func(scale * frequency * t + initialOffset);
    const signal = (funcValue & 255) / 127.5 - 1;
    out[i] = clamp(signal * 0.2, -0.4, 0.4);
    t++;
  }
  return out;
}
