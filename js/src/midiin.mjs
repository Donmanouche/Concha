// Concha's MIDI input: the DAW's MIDI in, with the rules of @strudel/midi (midin, midikeys).
//
// In a plug-in there is one input, the DAW's: midin('any name') and midikeys('any name') both read
// it (the name is kept for messages only).
//  - midin(name) -> (cc, chan?) => pattern of the last value of that CC (0..1), all channels or
//    channel chan (1..16). The values live in the plug-in (saved with the project), read through
//    __concha_control(0, cc, chan).
//  - midikeys(name) -> (noteLength = 0.5) => pattern of the notes played now: { midikey, note,
//    velocity, midichan }. As Strudel's _triggerKeyboard, each note-on is played at once: the
//    pattern thread calls keyQuery(), which queries the pattern at the note's time and keeps the
//    haps coming from midikeys. Regular queries don't see them (they were queried earlier).
//  - macro(n): Concha's 16 automatable macros (DAW parameters "Macro 1..16"), 0..1.
import { Hap, Pattern, TimeSpan, ref, reify, logger } from '@strudel/core';

// Live controls: query() records the controls a query reads (reads: key -> value), then queries again
// with one of them moved (override) to learn how each parameter follows it. Key: "kind:a:b".
export const live = { reads: null, override: null };

const control = (kind, a, b) => {
  const key = `${kind}:${a}:${b}`;
  if (live.override && live.override.key === key) return live.override.value;
  const value = typeof globalThis.__concha_control === 'function' ? globalThis.__concha_control(kind, a, b) : 0;
  if (live.reads && !live.reads.has(key)) live.reads.set(key, value);
  return value;
};

let announced = false;
function announce(input) {
  if (announced) return;
  announced = true;
  logger(`[midi] using the DAW's MIDI input${input !== undefined ? ` (for "${input}")` : ''}`);
}

export async function midin(input) {
  announce(input);
  return (cc, chan) => ref(() => control(0, Number(cc), chan === undefined ? 0 : Number(chan)));
}

// notes waiting for keyQuery, by input name
const pending = {};
const keyboards = new Set();

export async function midikeys(input) {
  announce(input);
  const name = String(input ?? 0);
  keyboards.add(name);
  pending[name] = [];
  return (noteLength = 0.5) => {
    const nlPat = reify(noteLength);
    return new Pattern((state) => {
      if (!state.controls?.conchaKeys) return [];
      return (pending[name] ?? []).flatMap((hap) =>
        nlPat.query(state.setSpan(hap.wholeOrPart())).map((lenHap) => {
          const nl = lenHap.value ?? 0.5;
          const whole = new TimeSpan(hap.whole.begin, hap.whole.begin.add(nl));
          const part = new TimeSpan(hap.part.begin, hap.part.begin.add(nl));
          return new Hap(whole, part, hap.value, hap.combineContext(lenHap));
        }),
      );
    });
  };
}

// A note-on from the DAW at `cycle`: queued for every keyboard; the caller then queries
// [cycle - eps, cycle + eps] with keysOnly. Returns false when no midikeys() is in use.
export function pushKey(note, velocity, channel, cycle) {
  if (!keyboards.size) return false;
  const span = new TimeSpan(cycle, cycle);
  for (const name of keyboards)
    pending[name].push(
      new Hap(span, span, { midikey: `${name}_${note}`, note: Math.round(note), velocity: velocity / 127, midichan: channel }, {}),
    );
  return true;
}

export function clearKeys() {
  for (const name of keyboards) pending[name] = [];
}

// a new evaluation forgets the keyboards of the old code
export function resetKeyboards() {
  keyboards.clear();
  for (const k of Object.keys(pending)) delete pending[k];
}

export function macro(n) {
  return ref(() => control(1, Number(n), 0));
}
