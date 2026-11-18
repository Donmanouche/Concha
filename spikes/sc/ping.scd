// Writes the spike's test SynthDef. usage: sclang ping.scd <output dir>
(
var dir = thisProcess.argv[0] ?? ".";
SynthDef(\concha_ping, { |out = 0, freq = 440, amp = 0.3|
    var env = EnvGen.kr(Env.perc(0.0005, 0.3), doneAction: 2);
    OffsetOut.ar(out, (SinOsc.ar(freq) * env * amp) ! 2); // OffsetOut: starts on the exact sample, as in SuperDirt
}).writeDefFile(dir);
"wrote %/concha_ping.scsyndef\n".postf(dir);
0.exit;
)
