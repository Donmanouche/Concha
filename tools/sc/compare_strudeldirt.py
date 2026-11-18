#!/usr/bin/env python3
"""Concha's StrudelDirt instruments against the real StrudelDirt: the same haps, the same synth controls?

For each Strudel file given, concha_render --trace-sc gives the haps that play a StrudelDirt
instrument and the controls Concha starts each synth with. The haps become the events Strudel's
OSC output sends (packages/osc/osc.mjs, parseControlsFromHap), which the real StrudelDirt
(third_party/strudeldirt, in the installed sclang, tools/sc/strudeldirt_events.scd) turns into
its /s_new messages. The instrument's controls are compared one by one.

    python3 tools/sc/compare_strudeldirt.py [--cycles N] [--cps X] [--vowel PATH] code.js...

The controls Concha leaves to superdough are not compared: out (each synth has its own bus) and pan
(the synth stays centred, superdough pans the voice). A control only one side sends is compared with
the SynthDef's default. Exit code 1 when a difference remains.
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKIP = {'out', 'pan'}
# StrudelDirt's effect modules Concha runs in scsynth too (src/sched/SoundConvert.cpp); the others
# (lpf, crush, delay...) are superdough's in Concha, and dirt_gate / strudel_envelope are the voice's
MODULES = {'dirt_pitch_shift2', 'dirt_grain_envelope2', 'dirt_tremolo2', 'waveloss2', 'squiz2', 'dirt_fshift2',
           'dirt_triode2', 'dirt_krush2', 'dirt_octer2', 'dirt_ring2', 'dirt_distort2', 'spectral-delay2',
           'spectral-freeze2', 'spectral-comb2', 'spectral-smear2', 'spectral-scram2', 'spectral-binshift2',
           'spectral-hbrick2', 'spectral-lbrick2', 'spectral-conformer2', 'spectral-enhance2'}


def note_to_midi(note, default_octave=3):
    m = re.match(r'^([a-gA-G])([#bsf]*)(-?[0-9]*)$', note)
    chroma = {'c': 0, 'd': 2, 'e': 4, 'f': 5, 'g': 7, 'a': 9, 'b': 11}[m.group(1).lower()]
    offset = sum({'#': 1, 's': 1, 'b': -1, 'f': -1}[a] for a in m.group(2))
    octave = int(m.group(3)) if m.group(3) else default_octave
    return (octave + 1) * 12 + chroma + offset


def parse_numeral(x):
    if isinstance(x, (int, float)):
        return x
    try:
        return float(x)
    except ValueError:
        return note_to_midi(x) if re.match(r'^[a-gA-G][#bsf]*-?[0-9]*$', x) else x


def controls_from_hap(t):
    """parseControlsFromHap: what Strudel's OSC output sends for a hap."""
    cps = t['cps']
    c = {'cps': cps, 'cycle': t['cycle'], 'delta': t['duration'] / cps}
    c.update(t['hap'])
    if c.get('n'):
        c['n'] = parse_numeral(c['n'])
    if 'note' in c:
        note = c['note']
        if isinstance(note, str) and re.match(r'^[a-gA-G][#bsf]*-?[0-9]*$', note):
            c['midinote'] = note_to_midi(note, c.get('octave') or 3)
        else:
            c['midinote'] = parse_numeral(note)
    if c.get('roomsize'):
        c['size'] = parse_numeral(c['roomsize'])
    if c.get('unit') == 'c' and c.get('speed') is not None:
        c['speed'] = c['speed'] / cps
    return c


def sc_value(v):
    if isinstance(v, bool):
        return '1' if v else '0'
    if isinstance(v, (int, float)):
        return repr(float(v)) if math.isfinite(v) else '0'
    return "'" + str(v).replace('\\', '\\\\').replace("'", "\\'") + "'"


def sc_event(c):
    return '(' + ', '.join(f'{k}: {sc_value(v)}' for k, v in c.items() if re.match(r'^[a-z]\w*$', k)) + ')'


def defaults():
    with open(os.path.join(ROOT, 'resources/strudeldirt/synthdefs/index.json')) as f:
        return {d['name']: d['controls'] for d in json.load(f)}


def num(x, default=0.0):
    return float(x) if isinstance(x, (int, float)) else default


def close(a, b):
    return abs(a - b) <= 1e-4 * max(1.0, abs(a), abs(b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cycles', default='2')
    ap.add_argument('--cps', default='0.5')
    ap.add_argument('--vowel', default=os.path.expanduser('~/.local/share/SuperCollider/downloaded-quarks/Vowel'))
    ap.add_argument('--render', default=os.path.join(ROOT, 'build/concha_render'))
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()
    defs = defaults()
    work = tempfile.mkdtemp(prefix='concha_dirt_')
    with open(os.path.join(ROOT, 'tools/sc/sclang_conf.yaml.in')) as f:
        conf = f.read().replace('@VOWEL@', a.vowel).replace('@STRUDELDIRT@', os.path.join(ROOT, 'third_party/strudeldirt'))
    conf_path = os.path.join(work, 'sclang_conf.yaml')
    with open(conf_path, 'w') as f:
        f.write(conf)

    haps, synths, owner, modules = [], [], [], []
    for path in a.files:
        trace = os.path.join(work, 'trace.jsonl')
        subprocess.run([a.render, '--cycles', a.cycles, '--cps', a.cps, '--trace-sc', trace, path, os.path.join(work, 'out.wav')],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # the haps (converted first), then the synths in the order the voices start, each followed
        # by its effect modules (Engine starts a voice's synth, then its effects chain)
        h, s = [], []
        with open(trace) as f:
            for line in f:
                j = json.loads(line)
                if 'hap' in j:
                    h.append(j)
                elif 'synth' in j:
                    j['modules'] = []
                    s.append(j)
                elif 'module' in j and s:
                    s[-1]['modules'].append(j)
        # a hap and its synth: the same onset, in order
        s.sort(key=lambda x: x['sample'])  # stable: the same order within a sample
        h.sort(key=lambda x: x['sample'])
        if len(h) != len(s):
            print(f'{path}: {len(h)} haps but {len(s)} synths started')
        for x, y in zip(h, s):
            haps.append(x)
            synths.append(y)
            owner.append(path)
            modules.append(y['modules'])

    events = os.path.join(work, 'events.scd')
    with open(events, 'w') as f:
        f.write('[\n' + ',\n'.join(sc_event(controls_from_hap(t)) for t in haps) + '\n]\n')
    dirt_out = os.path.join(work, 'dirt.jsonl')
    # an empty config folder: no startup.scd (a user's may boot a server and SuperDirt)
    os.makedirs(os.path.join(work, 'config'), exist_ok=True)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', XDG_CONFIG_HOME=os.path.join(work, 'config'))
    subprocess.run(['sclang', '-l', conf_path, os.path.join(ROOT, 'tools/sc/strudeldirt_events.scd'), events, dirt_out],
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    dirt = {}
    with open(dirt_out) as f:
        for line in f:
            j = json.loads(line)
            dirt[j['i']] = j['messages']

    bad = 0
    for i, (t, s, fx) in enumerate(zip(haps, synths, modules)):
        name = s['synth']
        ref = None
        for m in dirt.get(i, []):
            if m[0] == '/s_new' and m[1] == name:
                ref = dict(zip(m[5::2], m[6::2]))
        label = f"{os.path.basename(owner[i])} #{i} {name} {json.dumps(t['hap'])}"
        if ref is None:
            print(f'{label}\n    StrudelDirt played no {name}: {[m[:2] for m in dirt.get(i, [])]}')
            bad += 1
            continue
        mine = s['args']
        d = defs.get(name, {})
        diffs = []
        for k in sorted(set(ref) | set(mine)):
            if k in SKIP:
                continue
            r, c = ref.get(k, d.get(k)), mine.get(k, d.get(k))
            if not isinstance(r, (int, float)) or not isinstance(c, (int, float)):
                if r != c:
                    diffs.append(f'{k}: StrudelDirt {r} Concha {c}')
                continue
            if not close(float(r), float(c)):
                diffs.append(f'{k}: StrudelDirt {r:.6g} Concha {c:.6g}')
        # the envelope module and dirt_gate, which Concha applies in the voice
        env = t.get('envelope', {})
        mods = {m[1]: dict(zip(m[5::2], m[6::2])) for m in dirt.get(i, []) if m[0] == '/s_new' and m[1] != name}
        e = mods.get('strudel_envelope2')
        if e is None:
            diffs.append('StrudelDirt has no envelope module')
        else:
            want = {'attack': max(0.001, num(e.get('attack'))), 'decay': num(e.get('decay')), 'hold': num(e.get('hold')),
                    'release': num(e.get('release')), 'curve': num(e.get('curve')), 'amp': num(e.get('amp'), 0.3),
                    'holdtime': num(e.get('holdtime'))}
            for k, v in want.items():
                if not close(v, env.get(k, float('nan'))):
                    diffs.append(f'envelope {k}: StrudelDirt {v:.6g} Concha {env.get(k)}')
        g = mods.get('dirt_gate2')
        if g is None:
            diffs.append('StrudelDirt has no dirt_gate')
        else:
            if not close(num(g.get('totalDuration')), env.get('totalDuration', float('nan'))):
                diffs.append(f"dirt_gate totalDuration: StrudelDirt {g.get('totalDuration')} Concha {env.get('totalDuration')}")
            if num(g.get('fadeInTime')) != 0 or not close(num(g.get('fadeTime'), 0.001), 0.001):
                diffs.append(f"dirt_gate fades: StrudelDirt {g.get('fadeInTime')} / {g.get('fadeTime')}, Concha 0 / 0.001")
        # the effect modules: the same ones, in the same order, with the same controls
        theirs = [m for m in dirt.get(i, []) if m[0] == '/s_new' and m[1] in MODULES]
        if [m[1] for m in theirs] != [m['module'] for m in fx]:
            diffs.append(f"modules: StrudelDirt {[m[1] for m in theirs]} Concha {[m['module'] for m in fx]}")
        else:
            for m, c in zip(theirs, fx):
                r = dict(zip(m[5::2], m[6::2]))
                md = defs.get(m[1], {})
                for k in sorted(set(r) | set(c['args'])):
                    if k in ('in', 'out'):
                        continue
                    rv, cv = r.get(k, md.get(k)), c['args'].get(k, md.get(k))
                    if isinstance(rv, (int, float)) and isinstance(cv, (int, float)) and close(float(rv), float(cv)):
                        continue
                    if rv != cv:
                        diffs.append(f'{m[1]} {k}: StrudelDirt {rv} Concha {cv}')
        if diffs:
            bad += 1
            print(label + '\n    ' + '\n    '.join(diffs))
    print(f'{len(haps)} synths compared, {bad} with differences')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
