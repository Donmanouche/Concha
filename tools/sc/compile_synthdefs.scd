// Writes StrudelDirt's SynthDefs (third_party/strudeldirt, 2 channels) as .scsyndef files for
// Concha's embedded scsynth, with an index of their controls. Run with tools/sc/compile_synthdefs.sh
// (an installed SuperCollider and the Vowel quark are needed, only for this step).
// The server is not booted: SuperDirt.new only builds the SynthDefs (SynthDef.add keeps them in
// SynthDescLib when no server runs).
(
var outDir = thisProcess.argv[0] ?? { "resources/strudeldirt/synthdefs" };
var before = SynthDescLib.global.synthDescs.keys.copy;
var dirt, names, index;
File.mkdir(outDir);
dirt = SuperDirt(2, Server.default);
// Concha's own: a voice's audio into a private bus (where StrudelDirt's effect modules work) and back
SynthDef(\concha_fx_in2, { |in = 0, out = 0| ReplaceOut.ar(out, In.ar(in, 2)) }).add;
SynthDef(\concha_fx_out2, { |in = 0, out = 0| Out.ar(out, In.ar(in, 2)) }).add;
names = SynthDescLib.global.synthDescs.keys.reject { |k| before.includes(k) }.asArray.sort;
index = File(outDir +/+ "index.json", "w");
index.write("[\n");
names.do { |name, i|
	var desc = SynthDescLib.global.at(name);
	desc.def.writeDefFile(outDir);
	index.write("  {\"name\": \"%\", \"controls\": {".format(name));
	desc.controls.do { |c, j|
		var v = c.defaultValue;
		v = if(v.isArray) { v[0] } { v };
		index.write("%\"%\": %".format(if(j > 0) { ", " } { "" }, c.name, if(v.isNumber) { v } { 0 }));
	};
	index.write("}}%\n".format(if(i < (names.size - 1)) { "," } { "" }));
};
index.write("]\n");
index.close;
"wrote % SynthDefs to %".format(names.size, outDir).postln;
"modules: %".format(dirt.modules.collect(_.name)).postln;
0.exit;
)
