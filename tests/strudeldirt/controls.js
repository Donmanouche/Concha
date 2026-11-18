// The controls DirtEvent computes or passes on (tools/sc/compare_strudeldirt.py)
stack(
  s("superpiano*4").n("0 3 7 12").octave("<4 5>").velocity(0.5),
  s("superfm").note("c2 g2").clip(0.5).release(0.3).attack(0.05),
  s("supersaw supersquare").note("e3").legato(2).decay(0.1),
  s("superhammond").note("a3").hold(0.4).decay(0.2).curve(0).amp(0.5),
  s("superkick").speed("1 2").accelerate(-0.5),
  s("supergong").note("f#3 bb2").sustain(0.3).voice(0.7).detune(0.2),
  s("superpiano").note("c3").unit("c").speed(2),
  s("superreese").note("g1").n(2).lfo(2).rate(3)
).slow(2)
