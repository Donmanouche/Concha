#!/usr/bin/env python3
"""superdough C++ against the real superdough, case by case.

  python3 tests/ref/run_cases.py [filter]

For each case: the real superdough renders it (js/ref/dist/superdough-render.mjs, built by
`node js/ref/build-ref.mjs js/ref/dist/superdough-render.mjs` and
`node js/ref/build-worklets.mjs js/ref/dist/worklets.mjs`), concha_render renders it, and
compare.py measures the difference. Sampler cases use test files generated in build/ref/.
A case passes when env median <= 1 dB, env p95 <= `env95` dB (3 by default), spectrum <= `spec`
dB (3 by default), balance <= 0.5 dB and length within 30 ms.
"""
import os
import subprocess
import sys
import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '../..'))
out = os.path.join(root, 'build/ref')
os.makedirs(out, exist_ok=True)
sys.path.insert(0, here)
from compare import compare  # noqa: E402


def write_wav(path, x, sr=48000):
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1:
        x = x[:, None]
    ch = x.shape[1]
    data = x.tobytes()
    hdr = b'RIFF' + (36 + len(data)).to_bytes(4, 'little') + b'WAVEfmt ' + (16).to_bytes(4, 'little')
    hdr += (3).to_bytes(2, 'little') + ch.to_bytes(2, 'little') + sr.to_bytes(4, 'little')
    hdr += (sr * ch * 4).to_bytes(4, 'little') + (ch * 4).to_bytes(2, 'little') + (32).to_bytes(2, 'little')
    hdr += b'data' + len(data).to_bytes(4, 'little')
    open(path, 'wb').write(hdr + data)


# test samples: a decaying chirp (mono, 44.1 kHz: resampled) and a stereo tone pair (48 kHz)
t = np.arange(int(0.8 * 44100)) / 44100
write_wav(os.path.join(out, 'chirp.wav'), np.sin(2 * np.pi * (200 * t + 900 * t * t)) * np.exp(-3 * t) * 0.8, 44100)
t2 = np.arange(int(0.5 * 48000)) / 48000
write_wav(os.path.join(out, 'stereo.wav'), np.stack([np.sin(2 * np.pi * 330 * t2), 0.5 * np.sin(2 * np.pi * 550 * t2)], 1) * 0.7)
# a wavetable: 8 frames of 2048 samples morphing from sine to square-ish
frames = []
ph = np.arange(2048) / 2048
for k in range(8):
    frames.append(np.tanh((1 + 3 * k) * np.sin(2 * np.pi * ph)) / np.tanh(1 + 3 * k))
write_wav(os.path.join(out, 'table.wav'), np.concatenate(frames) * 0.9, 44100)
# an impulse response for ir(): deterministic decaying noise, stereo, 1.2 s at 48 kHz
rng = np.random.default_rng(7)
t3 = np.arange(int(1.2 * 48000)) / 48000
write_wav(os.path.join(out, 'ir.wav'), rng.uniform(-1, 1, (len(t3), 2)) * np.exp(-4 * t3)[:, None] * 0.5)
S = f"samples({{chirp: ['chirp.wav'], st: ['stereo.wav'], multi: ['chirp.wav', 'stereo.wav'], wt_test: ['table.wav'], irt: ['ir.wav']}}, 'file://{out}/');\n"

# Known difference of the reference: node-web-audio-api (Rust) does not normalise the band-limited
# square and sawtooth by their peak (Gibbs overshoot), the WebAudio spec and Chromium do (and so does
# Concha): Concha is 1.4 dB quieter on those two waves. For them the level is compensated before
# comparing ('level' tolerance), the shape must still match (correlation, spectrum).
RUST_NORM = {'level': 1.45}
NOISE = {'corr': None, 'noise': True, 'env_median': 1.5}
# reverbs: impulse responses are random noise (Math.random in superdough): compare levels and spectra
REVERB = {'corr': None, 'noise': True, 'spec_from': 500, 'spec': 3.5}

# (name, code, cycles, cps, extra tolerances)
cases = [
    ('sine notes', 'note("c3 e3 g3 c4").s("sine")', 2, 0.5, {}),
    ('saw ADSR', 'note("a2 e3").s("sawtooth").attack(.05).decay(.1).sustain(.4).release(.3)', 2, 0.5, RUST_NORM),
    ('square lpf', 'note("c2 c3").s("square").lpf(800).lpq(8)', 2, 0.5, RUST_NORM),
    ('triangle lpenv', 'note("e2*4").s("triangle").lpf(300).lpenv(4).lpa(.02).lpd(.1)', 1, 0.5, {}),
    ('saw lpenv negative', 'note("c3").s("sawtooth").lpf(4000).lpenv(-3).lpd(.3)', 1, 0.5, RUST_NORM),
    ('hpf + bandpass', 'note("c3 g3").s("sawtooth").hpf(600).hpq(3).bpf(1500)', 2, 0.5, RUST_NORM),
    ('24db lowpass', 'note("c3").s("sawtooth").lpf(500).ftype("24db")', 1, 0.5, RUST_NORM),
    ('pan', 'note("c3 e3 g3 c4").s("sawtooth").pan("0 .25 .75 1").lpf(2000)', 1, 0.5, RUST_NORM),
    ('gain velocity postgain', 'note("c3*4").s("sine").gain("1 .5").velocity(".8 .3").postgain(1.5)', 1, 0.5, {}),
    ('vibrato', 'note("a3").s("sine").vib(6).vibmod(.5)', 1, 0.5, {}),
    ('pitch envelope', 'note("c3*2").s("sine").penv(12).pdecay(.2)', 1, 0.5, {}),
    ('fm', 'note("c3 g3").s("sine").fm(3).fmh(2)', 2, 0.5, {}),
    ('fm envelope', 'note("c3").s("sine").fm(4).fmh(1.5).fmattack(.2).fmdecay(.3).fmsustain(0)', 1, 0.5, {}),
    ('clip', 'note("c3 e3").s("sawtooth").clip(.25)', 1, 0.5, RUST_NORM),
    ('white noise', 's("white*4").decay(.4).sustain(0)', 4, 0.5, NOISE),
    ('pink noise lpf', 's("pink").lpf(2000)', 2, 0.5, dict(NOISE, env95=4)),  # one draw of noise per render
    ('brown noise', 's("brown*4")', 2, 0.5, NOISE),
    ('sample', S + 's("chirp")', 1, 0.5, {}),
    ('sample speed', S + 's("chirp*2").speed("2 .5")', 1, 0.5, {}),
    ('sample reverse', S + 's("chirp").speed(-1)', 1, 0.5, {}),
    ('sample begin/end', S + 's("chirp").begin(.25).end(.5)', 1, 0.5, {}),
    ('sample note', S + 's("chirp").note("c4")', 1, 0.5, {}),
    ('sample loop', S + 's("chirp").loop(1).loopBegin(.1).loopEnd(.3).release(.1)', 1, 0.5, {}),
    ('sample cut', S + 's("chirp*4").cut(1)', 1, 0.5, {}),
    ('sample stereo + pan', S + 's("st").pan(.2)', 1, 0.5, {}),
    ('sample n', S + 's("multi:0 multi:1")', 1, 0.5, {}),
    ('sample ADSR clip', S + 's("chirp*2").clip(.5).attack(.05).release(.1)', 1, 0.5, {}),
    # ---- phase 4a ----
    ('delay', 'note("c3 ~ ~ ~").s("sine").delay(.5)', 2, 0.5, {}),
    ('delay time feedback', 'note("e3 ~ ~ ~").s("triangle").delay(.6).delaytime(.25).delayfeedback(.7)', 2, 0.5, {}),
    # reverbs: both impulse responses come from the same pseudo-random noise (see js/ref/build-ref.mjs);
    # sources without randomness. Square waves: the reference's 1.4 dB normalisation difference.
    ('room', 'note("c1 ~ ~ ~").s("square").decay(.05).sustain(0).room(1).dry(0)', 2, 0.5, dict(RUST_NORM, len=150)),
    ('room size lp dim', 'note("c1 ~ ~ ~").s("square").decay(.05).sustain(0).room(1).dry(0).roomsize(4).roomlp(5000).roomdim(500)', 3, 0.5, RUST_NORM),
    ('room fade', 'note("c1 ~ c2 ~").s("square").decay(.05).sustain(0).room(1).dry(0).roomsize(1).roomfade(.5)', 2, 0.5, dict(RUST_NORM, len=150)),
    ('room with dry', 'note("c3 ~ ~ ~").s("sine").room(.4)', 2, 0.5, {}),
    ('vowel a', 'note("c3").s("sawtooth").vowel("a")', 1, 0.5, dict(RUST_NORM, len=150)),
    ('djf low', 'note("c3 e3 g3 c4").s("sawtooth").djf(.2)', 1, 0.5, RUST_NORM),
    ('djf high', 'note("c3 e3 g3 c4").s("sawtooth").djf(.8)', 1, 0.5, RUST_NORM),
    # high-Q band-passes: the very end of their ringing (-60 dB) differs slightly from the Rust reference
    ('vowel', 'note("c3*4").s("sawtooth").vowel("<a e i o>")', 4, 1, dict(RUST_NORM, corr=0.98, len=150)),
    ('coarse', 'note("c4").s("sine").coarse(8)', 1, 0.5, {}),
    ('crush', 'note("c3").s("sine").crush(3)', 1, 0.5, {}),
    ('shape', 'note("c3").s("sine").shape(.8)', 1, 0.5, {}),
    ('distort', 'note("c3 e3").s("sine").distort("2 4").distorttype("<0 1 2 3 4 5 6 7 8>")', 9, 0.5, {}),
    ('tremolo', 'note("c3").s("sine").tremolo(6).tremolodepth(.8)', 1, 0.5, {}),
    ('tremolo shape', 'note("c3").s("triangle").tremolo(4).tremoloshape("square")', 1, 0.5, {}),
    ('phaser', 'note("c3").s("sawtooth").phaser(2).phaserdepth(.8)', 1, 0.5, RUST_NORM),
    ('filter lfo', 'note("c3").s("sawtooth").lpf(800).lprate(4).lpdepth(.5)', 1, 0.5, RUST_NORM),
    ('ladder', 'note("c2 c3").s("sawtooth").lpf(600).lpq(8).ftype("ladder")', 2, 0.5, RUST_NORM),
    # random voice phases (Math.random): long render, compared like noise
    # (the beating of detuned voices changes with the phases: envelopes differ, levels and spectra must not)
    ('supersaw', 'note("c3*8").s("supersaw").lpf(3000)', 4, 0.5, dict(NOISE, env_median=3, env95=8)),
    ('supersaw unison spread', 'note("a2*8").s("supersaw").unison(3).spread(1).detune(.5)', 4, 0.5, dict(NOISE, env_median=3, env95=8)),
    ('pulse', 'note("c3 g3").s("pulse").pw(.3)', 1, 0.5, {}),
    ('pulse pwm', 'note("c3").s("pulse").pwrate(2).pwsweep(.4)', 1, 0.5, {}),
    ('sbd', 's("sbd*2")', 1, 0.5, dict(NOISE, spec=5)),  # random noise click: its spectrum varies per draw
    ('partials n', 'note("c3 e3").s("sawtooth").n("4 8")', 1, 0.5, {}),
    ('partials list', 'note("c3").s("user").partials([1, .5, .25, .5])', 1, 0.5, {}),
    # not comparable offline: superdough ducks from an onended callback (t - 10 ms) that an OfflineAudioContext
    # runs at arbitrary points of the render; checked by tests/superdough_tests.cpp instead
    ('duckorbit', 'stack(note("c3").s("sine").orbit(2).release(2), s("sine*4").note("c5").orbit(1).duckorbit(2).duckattack(.1).duckdepth(.8))', 1, 0.5, {'skip': True}),
    # ---- phase 4b ----
    # DynamicsCompressor pre-delay: 0.006 s = 288 samples in Chromium (and Concha), 384 in the Rust reference
    ('compressor', 'note("c3 e3").s("sine").gain(1.5).compressor(-20)', 1, 0.5, {'shift': 96}),
    ('compressor ratio knee', 'note("c2*4").s("sawtooth").compressor(-30).compressorRatio(20).compressorKnee(0).compressorAttack(.01).compressorRelease(.2)', 1, 0.5, dict(RUST_NORM, shift=96)),
    ('zzfx', 'note("c3 e3 g3").s("z_sawtooth").decay(.1)', 1, 0.5, {}),
    ('zzfx noise', 's("z_noise").zcrush(.5)', 1, 0.5, NOISE),
    ('bytebeat', 'note("c3 g3").s("bytebeat").n("<0 1 2>")', 3, 0.5, {}),
    ('bytebeat expression', "note(\"c3\").s(\"bytebeat\").byteBeatExpression('t*(t>>9|t>>13)&255')", 1, 0.5, {}),
    ('wavetable', S + 'note("c3 g3").s("wt_test")', 1, 0.5, {}),
    ('wavetable position env', S + 'note("c3").s("wt_test").wt(0).wtenv(1).wtattack(.5)', 1, 0.5, {}),
    ('wavetable lfo warp', S + 'note("c3").s("wt_test").wt(.3).wtrate(2).wtdepth(.5).warp(.4).warpmode("<asym mirror fold pwm spin binary>")', 6, 0.5, {}),
    # random voice phases (Math.random() * phaserand): compared like supersaw
    ('wavetable unison', S + 'note("c3*8").s("wt_test").unison(4).spread(.8).detune(.3)', 4, 0.5, dict(NOISE, env_median=8, env95=18, bal=1.0, spec=4)),
    ('transient', S + 's("chirp*2").transient(".8:-.5")', 1, 0.5, {}),
    # lfo / env modulators (modulators.mjs)
    ('lfo cutoff', 'note("F1").s("sawtooth").lpf(500).lfo({rate: 2})', 1, 0.5, RUST_NORM),
    ('lfo sync gain', 'note("c3 e3").s("triangle").gain(.8).lfo({s: 4, depth: .5, sh: 1})', 1, 0.5, {}),
    ('lfo pitch', 'note("c4").s("sine").lfo({c: "s", r: 5, da: 20})', 1, 0.5, {}),
    ('lfo on lfo', 'note("F1").s("sawtooth").lpf(400).lfo({rate: 1}).lfo({c: "lfo_0", r: .5, da: 3})', 1, 0.5, RUST_NORM),
    # envelope-processor never triggers for an event at currentTime 0 (begin == its initial beginTime):
    # the env cases start a little later, as any event does in a live context
    ('env cutoff', 'note("F1 c2").s("sawtooth").lpf(300).env({a: .1, d: .2, depth: 4}).late(.125)', 1, 0.5, RUST_NORM),
    ('env curves pan', 'note("c3*2").s("triangle").pan(.2).env({c: "pan", a: .2, d: .3, s: .5, da: .6, ac: .8, dc: -.5}).late(.125)', 1, 0.5, {}),
    ('ir', S + 's("chirp").room(.5).ir("irt")', 1, 0.5, {}),
    ('ir speed begin size', S + 's("chirp").room(.6).ir("irt").irspeed(2).irbegin(.1).roomsize(.5)', 1, 0.5, {}),
    ('ir reversed', S + 's("chirp").room(.6).ir("irt").irspeed(.5)', 1, 0.5, {}),
    # FM matrix (applyFM): chains, cross routes, per-operator envelopes and waves, a muted cycle
    # one note from the start: node-web-audio-api accumulates oscillator phases in float32 from the
    # context's start (Chromium and Concha: double), which a deep FM chain amplifies on later notes
    ('fm chain', 'note("c3").s("sine").fmi(2).fmh(2).fmi2(1.5).fmh2(3).fmi3(.8).fmh3(.5)', 1, 0.5, {}),
    ('fm cross env wave', 'note("c3").s("triangle").fmi(1).fmh(1.01).fm31(2).fmh3(4).fmattack3(.3).fmdecay3(.2).fmsustain3(.2).fmwave3("square").fmenv("lin").fmattack(.1)', 1, 0.5, {}),
    ('fm cycle muted', 'note("c3").s("sine").fmi(1).fmh(2).fmi2(1).fmi12(1)', 1, 0.5, {}),
    ('fm lfo index', 'note("c3").s("sine").fmi(1).fmh(2).lfo({c: "fmi", r: 2, da: 2})', 1, 0.5, {}),
    # audio buses: .bus(n) sends, s("bus") reads, bmod modulates
    ('bus read', 'stack(note("c3 e3").s("sawtooth").bus(1).dry(0), s("bus").n(1).lpf(800).attack(.05))', 1, 0.5, RUST_NORM),
    ('bus busgain stereo', 'stack(note("c3").s("triangle").pan(.2).bus(2).busgain(.5), s("bus").n(2).delay(.3))', 1, 0.5, {}),
    ('bmod cutoff', 'stack(note("c1").s("sine").bus(3).dry(0), note("c3").s("sawtooth").lpf(600).bmod({bus: 3, depth: 2}))', 1, 0.5, {'skip': True}),  # connected from an onended callback
    ('bmod gain dc', 'stack(note("c0").s("triangle").bus(4).dry(0), note("e3").s("sine").gain(.5).bmod({bus: 4, dc: .3, da: .3}))', 1, 0.5, {'skip': True}),
    # modulation targets of the end of phase 4 (worklet parameters read per quantum or per sample)
    ('mod coarse', 'note("c3").s("sawtooth").coarse(4).lfo({r: 2, da: 6})', 1, 0.5, RUST_NORM),
    ('mod crush', 'note("c3").s("triangle").crush(6).lfo({r: 1, da: 3})', 1, 0.5, {}),
    ('mod shape shapevol', 'note("c3").s("triangle").shape(.5).lfo({c: "shape", r: 3, da: .4}).lfo({c: "shapevol", r: 1, da: .3})', 1, 0.5, {}),
    ('mod distort', 'note("c3").s("triangle").distort(1).lfo({r: 2, da: 1})', 1, 0.5, {}),
    ('mod compressor', 'note("c3 e3").s("sawtooth").compressor(-20).lfo({c: "compressor", r: 2, da: 15})', 1, 0.5, dict(RUST_NORM, shift=96)),
    ('mod delaytime feedback', 'note("c3 ~ ~ ~").s("sine").delay(.5).delaytime(.1).delayfeedback(.5).lfo({c: "delaytime", r: .5, da: .05}).lfo({c: "delayfeedback", r: 1, da: .3})', 1, 0.5, {}),
    ('mod djf', 'note("c3").s("sawtooth").djf(.3).lfo({c: "djf", r: 1, da: .3})', 1, 0.5, RUST_NORM),
    # the end of the high-Q band-passes' ringing differs a little (see 'vowel'): 'len'
    ('mod vowel', 'note("c3").s("sawtooth").vowel("a").lfo({r: 2, da: 200})', 1, 0.5, dict(RUST_NORM, corr=0.98, len=100)),
    ('mod filter lfo depth', 'note("c2").s("sawtooth").lpf(400).lprate(2).lpdepth(1).lfo({c: "lpdepth", r: .5, da: 1})', 1, 0.5, RUST_NORM),
    ('mod env attack', 'note("c2").s("sawtooth").lpf(300).env({a: .3, d: .3, depth: 4}).lfo({c: "env_0", sc: "attack", r: 4, da: .2}).late(.125)', 1, 0.5, RUST_NORM),
    ('mod lfo curve', 'note("c2").s("sawtooth").lpf(500).lfo({r: 2, dc: 0}).lfo({c: "lfo_0", sc: "curve", r: 1, da: .5})', 1, 0.5, RUST_NORM),
    ('mod tremoloskew', 'note("c3").s("triangle").tremolo(4).lfo({c: "tremoloskew", r: .5, da: .4})', 1, 0.5, {}),
    ('mod phaserrate', 'note("c3").s("sawtooth").phaser(2).lfo({c: "phaserrate", r: .5, da: 1})', 1, 0.5, RUST_NORM),
    ('mod pwsweep', 'note("c3").s("pulse").pwrate(1).pwsweep(.3).lfo({c: "pwsweep", r: .5, da: .2})', 1, 0.5, {}),
    ('mod stretch', 'note("c3").s("triangle").stretch(.5).lfo({c: "stretch", r: 1, da: .3}).late(.125)', 1, 0.5, {'len': 60}),
    ('mod in FX stage', 'note("c2").s("sawtooth").FX(lpf(400).lfo({r: 2, depth: 1})).gain(.8)', 1, 0.5, RUST_NORM),
    ('mod fxi from main', 'note("c3").s("triangle").FX(distort(.5)).lfo({c: "distort", fxi: 0, r: 2, da: .5})', 1, 0.5, {}),
    # FX chains: each stage runs the whole effect block (default gain 0.8 per stage), stages before
    # the main one have their own delay and reverb
    ('FX coarse lpf distort', 'note("c2 g2").s("sawtooth").FX(coarse(4)).FX(lpf(500).lpe(4).lpa(.1).lpd(.2)).FX(distort(1))', 1, 0.5, RUST_NORM),
    ('FX gain phaser bpf', 'note("c3").s("triangle").FX(phaser(.5).gain(2), bpf(800)).gain(.6)', 1, 0.5, {}),
    ('FX stage delay', 'note("c3 ~ ~ ~").s("sine").FX(delay(.5).delaytime(.1).delayfeedback(.6).gain(1.25)).fxr(1)', 1, 0.5, {}),
    ('FX stage room', 'note("c3 ~ ~ ~").s("triangle").FX(room(.5).roomsize(1)).fxr(1.5)', 1, 0.5, REVERB),
    ('FX K', 'note("c3").s("sine").FX(K(() => { audioin().fold(.5).out() })).lpf(2000)', 1, 0.5, {}),
    # stretch: phase vocoder (superdough starts the sound 40 ms earlier; t - 0.04 must not be < 0).
    # The worklet stops when its input goes away: in Chromium (and Concha) when the source stops, in
    # node-web-audio-api only after its overlap-add tail (2048 samples): 'len' allows that.
    ('stretch up', 'note("c3").s("sawtooth").stretch(1).late(.125)', 1, 0.5, dict(RUST_NORM, len=60)),
    ('stretch down sample', S + 's("chirp").stretch("-1").late(.125)', 1, 0.5, {'len': 60}),
    # identical while the sample plays; the cut at its end (vs the reference's tail) shows in the spectrum
    ('stretch stereo', S + 's("st").stretch(.5).late(.125)', 1, 0.5, {'len': 60, 'spec': 8, 'bal': 0.6, 'corr': 0.9}),
    # K(): kabelsalat graphs (generic-processor)
    ('K sine adsr', 'note("c3 e3").s("sine").K(() => { sine(sFreq).mul(sGate.adsr(.01, .1, .5, .2)).mul(.5).out() })', 1, 0.5, {}),
    ('K delay feedback', 'note("c3 ~ g3 ~").s("sine").K(() => { tri(sFreq).mul(sGate.ad(.01, .2)).add(x => x.delay(.15).mul(.6)).mul(.4).out() }).fxr(1)', 1, 0.5, {}),
    ('K audioin filter', 'note("c2").s("triangle").K(() => { audioin().filter(sine(2).range(.3, .8), .5).out() })', 1, 0.5, {}),
    ('K pattern inputs', 'note("c3*4").s("sine").K(() => { pulse(sFreq, S("<.2 .5>")).mul(sGate.ad(.005, .1)).mul(.2).out() })', 2, 0.5, {}),
    ('K seq fold qf', 'note("c3").s("sine").K(() => { impulse(8).seq(220, 330, 440).sine().fold(.5).qf(0, 2000, 3).mul(sGate.adsr(.01, .1, .8, .1)).mul(.3).out() })', 1, 0.5, {}),
    ('K logic pan', 'note("c3").s("sine").K(() => { let s = zaw(110).greater(0).ifelse(sine(220), tri(330)); s.pan(sine(1)).mul(.3).out() })', 1, 0.5, {}),
    ('env sample pitch', S + 's("chirp").env({c: "s", a: .05, d: .3, da: 700}).late(.125)', 1, 0.5, {}),
]


def run(cmd, stdin=None):
    return subprocess.run(cmd, input=stdin, capture_output=True, text=True, cwd=root)


flt = sys.argv[1] if len(sys.argv) > 1 else ''
ref_js = os.path.join(root, 'js/ref/dist/superdough-render.mjs')
concha = os.path.join(root, 'build/concha_render')
passed = failed = 0
for i, (name, code, cycles, cps, tol) in enumerate(cases):
    if flt and flt not in name:
        continue
    if tol.get('skip'):
        print(f'skip  {name:24s} (not comparable with an offline render)')
        continue
    code_path = os.path.join(out, f'case{i}.js')
    open(code_path, 'w').write(code)
    ref_wav, test_wav = os.path.join(out, f'case{i}-ref.wav'), os.path.join(out, f'case{i}-concha.wav')
    r1 = run(['node', ref_js, code_path, ref_wav, str(cycles), str(cps)])
    r2 = run([concha, '--cycles', str(cycles), '--cps', str(cps), code_path, test_wav])
    if r1.returncode or r2.returncode:
        print(f'ERROR  {name}\n  ref: {r1.stderr.strip()[-300:]}\n  concha: {r2.stderr.strip()[-300:]}')
        failed += 1
        continue
    m = compare(ref_wav, test_wav, noise=tol.get('noise', False), spec_from=tol.get('spec_from', 200), shift=tol.get('shift', 0))
    level = tol.get('level', 0.0)
    if level:  # known level difference of the reference: compare the shape
        m['env_median'] = abs(m['env_median'] - level)
        m['env_p95'] = abs(m['env_p95'] - level)
        m['spec_max'] = max(0.0, m['spec_max'] - level)
    ok = (m['env_median'] <= tol.get('env_median', 1.0) and m['env_p95'] <= tol.get('env95', 3.0) and m['spec_max'] <= tol.get('spec', 3.0)
          and m['bal'] <= tol.get('bal', 0.5) and m['len_ms'] <= tol.get('len', 150 if tol.get('noise') else 30)
          and (tol.get('corr', 1) is None or m['corr'] >= (tol['corr'] if isinstance(tol.get('corr'), float) else 0.99)))
    passed += ok
    failed += not ok
    corr = '' if tol.get('corr', 1) is None else f" corr {m['corr']:.3f}"
    print(f"{'ok    ' if ok else 'DIFF  '}{name:24s} env {m['env_median']:.2f}/{m['env_p95']:.2f} dB  spec {m['spec_max']:.2f} dB  "
          f"bal {m['bal']:.2f} dB  len {m['len_ms']:.0f} ms{corr}  peak {m['ref_peak']:.3f}/{m['test_peak']:.3f}")
print(f'\n{passed} close to superdough, {failed} different')
sys.exit(1 if failed else 0)
