// The messages the real StrudelDirt sends for a list of events, as Strudel's OSC output sends them
// (tools/sc/compare_strudeldirt.py writes the events). No server runs: SuperDirt talks to a
// "server" whose address is a port of this sclang, and every message that arrives is recorded.
//
//   sclang -l <conf with Vowel and StrudelDirt> strudeldirt_events.scd <events.scd> <out.jsonl>
//
// events.scd evaluates to an Array of Events (the controls of parseControlsFromHap). out.jsonl gets
// one line per event: {"i": index, "messages": [[address, args...], ...]}.
(
var argv = thisProcess.argv, inPath = argv[0], outPath = argv[1];
var port = 57999, log = List.new, server, dirt, events, out, json, next;
json = { |x|
	case
	{ x.isKindOf(Symbol) or: { x.isKindOf(String) } } { "\"" ++ x.asString.escapeChar($") ++ "\"" }
	{ x.isKindOf(Float) and: { x.isNaN } } { "null" }
	{ x.isKindOf(SimpleNumber) } { x.asFloat.asStringPrec(12) }
	{ x.isKindOf(Int8Array) } { "\"<" ++ x.size ++ " bytes>\"" }
	{ x.isKindOf(ArrayedCollection) } { "[" ++ x.collect(json).join(",") ++ "]" }
	{ "\"" ++ x.asString.escapeChar($") ++ "\"" }
};
thisProcess.openUDPPort(port);
server = Server(\conchaCompare, NetAddr("127.0.0.1", port));
Server.default = server;
dirt = SuperDirt(2, server); // its SynthDefs stay in sclang (no server running: no /d_recv)
// DirtOrbit wants a running server: say it is, without the boot actions
server.statusWatcher.instVarPut(\hasBooted, true).instVarPut(\notified, true)
	.instVarPut(\sampleRate, 48000).instVarPut(\actualSampleRate, 48000);
dirt.makeOrbits([0]);
events = inPath.load;
out = File(outPath, "w");
// one event at a time: play it, then a marker; when the marker comes back, all its messages have
// (sclang's clocks are late after the class library's compilation: no waiting on them)
next = { |i|
	if(i < events.size) {
		log.clear;
		// as SuperDirt's OSC function: the event's pairs, no latency (the order is all that counts)
		DirtEvent(dirt.orbits[0], dirt.modules, events[i].put(\latency, 0)).play;
		NetAddr("127.0.0.1", port).sendMsg('/concha_done', i);
	} {
		out.close;
		0.exit;
	}
};
thisProcess.addOSCRecvFunc({ |msg, time, addr, recvPort|
	if(recvPort == port and: { msg.size > 0 }) {
		case
		{ msg[0] == '/concha_done' } {
			if(msg[1] >= 0) { out.write("{\"i\":" ++ msg[1] ++ ",\"messages\":[" ++ log.collect(json).join(",") ++ "]}\n") };
			{ next.(msg[1] + 1) }.defer;
		}
		{ msg[0] != '/d_recv' and: { msg[0].asString.size > 0 } } { log.add(msg) }
	}
});
NetAddr("127.0.0.1", port).sendMsg('/concha_done', -1); // after the orbits' messages
)
