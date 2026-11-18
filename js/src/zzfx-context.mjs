// Stands in for superdough's audioContext.mjs inside zzfx_fork.mjs: buildSamples() only reads the
// sample rate, which is the engine's (set by Concha's pattern thread).
export const getAudioContext = () => ({ sampleRate: globalThis.__conchaSampleRate ?? 48000 });
