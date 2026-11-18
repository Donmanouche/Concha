// Concha's .midi(): the rules of @strudel/midi (packages/midi/midi.mjs, AGPL-3.0, Strudel
// contributors), but instead of WebMIDI the messages go to the DAW through the plug-in's MIDI
// output. Each message becomes an outbox event { kind: 'midimsg', begin (cycles), value }:
//   value.bytes: the raw MIDI bytes (status first; a whole F0 ... F7 for sysex)
// The C++ scheduler places them on the exact sample.
//
// Differences with WebMIDI on purpose:
//  - one output, the DAW's: the port argument / .midiport() are accepted and ignored;
//  - no note-off offset: note-offs come exactly at the end of the hap, and on the same sample the
//    scheduler sends note-offs before note-ons (that is what noteOffsetMs works around);
//  - midicmd clock/start/stop/continue are ignored: the DAW is the clock (real-time messages
//    cannot be sent through a plug-in's MIDI output anyway).
import { Pattern, isPattern, getControlName, noteToMidi, logger } from '@strudel/core';
import { outbox } from './stubs.mjs';

export const midicontrolMap = new Map();

function unifyMapping(mapping) {
  return Object.fromEntries(
    Object.entries(mapping).map(([key, m]) => [getControlName(key), typeof m === 'number' ? { ccn: m } : m]),
  );
}

export function defaultmidimap(mapping) {
  midicontrolMap.set('default', unifyMapping(mapping));
}

export async function midimaps(map) {
  if (typeof map === 'string') {
    // github:/URL maps need the network: phase 3 (sample library and cache) brings fetching.
    logger(`[midi] midimaps('${map}'): loading maps from a URL is not available yet in Concha`, 'warning');
    return;
  }
  if (typeof map === 'object') Object.entries(map).forEach(([name, m]) => midicontrolMap.set(name, unifyMapping(m)));
}

function normalize(value = 0, min = 0, max = 1, exp = 1) {
  if (min === max) throw new Error('min and max cannot be the same value');
  const n = Math.min(1, Math.max(0, (value - min) / (max - min)));
  return Math.pow(n, exp);
}

function mapCC(mapping, value) {
  return Object.keys(value)
    .filter((key) => !!mapping[getControlName(key)])
    .map((key) => {
      const { ccn, min = 0, max = 1, exp = 1 } = mapping[key];
      return { ccn, ccv: normalize(value[key], min, max, exp) };
    });
}

const byte = (v) => Math.max(0, Math.min(127, Math.round(v)));
const isByte = (b) => Number.isInteger(b) && b >= 0 && b <= 255;

function channelOf(midichan) {
  const c = Math.round(Number(midichan));
  if (!(c >= 1 && c <= 16)) throw new Error(`midichan must be between 1 and 16, got ${midichan}`);
  return c - 1;
}

// Builds the messages of one hap (same order and checks as @strudel/midi).
export function hapToMidi(hap, cps, config) {
  hap.ensureObjectValue?.();
  const v = hap.value;
  let {
    note, nrpnn, nrpv, ccn, ccv, midichan = config.midichannel, midicmd, midibend, miditouch,
    gain = config.gain, velocity = config.velocity, progNum, sysexid, sysexdata, midimap = config.midimap,
  } = v;
  const begin = hap.whole.begin.valueOf();
  const end = hap.whole.end.valueOf();
  const ch = channelOf(midichan);
  const out = [];
  const send = (bytes, at = begin) => out.push({ kind: 'midimsg', begin: at, end: at, value: { bytes }, locs: [] });
  velocity = gain * velocity;

  if (midicontrolMap.has(midimap)) {
    for (const cc of mapCC(midicontrolMap.get(midimap), v)) sendCC(cc.ccn, cc.ccv);
  } else if (midimap !== 'default') {
    logger(`[midi] midimap "${midimap}" not found! Available maps: ${[...midicontrolMap.keys()].join(', ')}`, 'warning');
  }

  function sendCC(n, value) {
    if (typeof value !== 'number' || value < 0 || value > 1) throw new Error('expected ccv to be a number between 0 and 1');
    if (typeof n !== 'number') throw new Error('expected ccn to be a number (0-127)');
    send([0xb0 | ch, byte(n), byte(value * 127)]);
  }

  if (note !== undefined && !config.isController) {
    if (note == null || note === '') throw new Error('note cannot be null or empty');
    if (velocity != null && (typeof velocity !== 'number' || velocity < 0 || velocity > 1))
      throw new Error('velocity must be a number between 0 and 1');
    const midiNumber = byte(typeof note === 'number' ? note : noteToMidi(note));
    send([0x90 | ch, midiNumber, Math.max(1, byte(velocity * 127))]);
    send([0x80 | ch, midiNumber, 0], end);
  }
  if (progNum !== undefined) {
    if (typeof progNum !== 'number' || progNum < 0 || progNum > 127)
      throw new Error('expected progNum (program change) to be a number between 0 and 127');
    send([0xc0 | ch, byte(progNum)]);
  }
  if (sysexid !== undefined && sysexdata !== undefined) {
    const id = Array.isArray(sysexid) ? sysexid : [sysexid];
    if (!id.every(isByte)) throw new Error('sysexid must be a number between 0 and 255 or an array of such numbers');
    if (!Array.isArray(sysexdata) || !sysexdata.every(isByte)) throw new Error('expected sysex to be an array of numbers (0-255)');
    send([0xf0, ...id, ...sysexdata, 0xf7]);
  }
  if (ccv !== undefined && ccn !== undefined) sendCC(ccn, ccv);
  if (nrpnn !== undefined && nrpv !== undefined) {
    const [pm, pl] = Array.isArray(nrpnn) ? nrpnn : [(nrpnn >> 7) & 127, nrpnn & 127];
    const [dm, dl] = Array.isArray(nrpv) ? nrpv : [nrpv, undefined];
    send([0xb0 | ch, 99, byte(pm)]);
    send([0xb0 | ch, 98, byte(pl)]);
    send([0xb0 | ch, 6, byte(dm)]);
    if (dl !== undefined) send([0xb0 | ch, 38, byte(dl)]);
  }
  if (midibend !== undefined) {
    if (typeof midibend !== 'number' || midibend < -1 || midibend > 1) throw new Error('expected midibend to be a number between -1 and 1');
    const v14 = Math.max(0, Math.min(16383, Math.round(((midibend + 1) / 2) * 16383)));
    send([0xe0 | ch, v14 & 127, v14 >> 7]);
  }
  if (miditouch !== undefined) {
    if (typeof miditouch !== 'number' || miditouch < 0 || miditouch > 1) throw new Error('expected miditouch to be a number between 0 and 1');
    send([0xd0 | ch, byte(miditouch * 127)]);
  }
  if (Array.isArray(midicmd)) {
    if (midicmd[0] === 'progNum') send([0xc0 | ch, byte(midicmd[1])]);
    else if (midicmd[0] === 'cc' && midicmd.length === 3) send([0xb0 | ch, byte(midicmd[1]), byte(midicmd[2])]);
    else if (midicmd[0] === 'sysex' && midicmd.length === 3) send([0xf0, ...[].concat(midicmd[1]), ...midicmd[2], 0xf7]);
  }
  return out;
}

Pattern.prototype.midi = function (midiport, options = {}) {
  if (isPattern(midiport)) {
    throw new Error(".midi does not accept Pattern input for midiport. Pass the device name with single quotes, e.g. .midi('out')");
  }
  if (typeof midiport === 'object' && midiport !== null) {
    const { port, isController = false, ...rest } = midiport;
    options = { isController, ...rest, ...options };
    midiport = port;
  }
  const config = { isController: false, midichannel: 1, velocity: 0.9, gain: 1, midimap: 'default', midiport, ...options };
  return this.sortHapsByPart().onSchedule((hap, t, cps) => {
    for (const e of hapToMidi(hap, cps, config)) outbox.events?.push(e);
  }, true);
};
