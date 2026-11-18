// Stands in for superdough's analysers (bundled as 'superdough' for webaudio's scope.mjs and
// spectrum.mjs): analysers[id] and getAnalyzerData(type, id) read the audio the plug-in captured
// for `.analyze(id)` (superdough: the voice after postgain, mono, fftSize 2^(fft + 5)) through
// __concha_analyser(id, kind) -> Float32Array (time: last fftSize/2 samples; frequency: dB, as an
// AnalyserNode with smoothing 0.5), or undefined before any sound.
export const analysers = new Proxy(
  {},
  {
    get(target, id) {
      if (typeof id !== 'string') return target[id];
      if (typeof globalThis.__concha_analyser !== 'function') return undefined;
      const size = globalThis.__concha_analyser(String(id), 'size');
      return size ? { fftSize: size, frequencyBinCount: size / 2 } : undefined;
    },
  },
);

export function getAnalyzerData(type = 'time', id = 1) {
  if (type !== 'time' && type !== 'frequency') throw new Error(`getAnalyzerData: ${type} not supported. use one of time, frequency`);
  if (typeof globalThis.__concha_analyser !== 'function') return new Float32Array(0);
  return globalThis.__concha_analyser(String(id), type) ?? new Float32Array(0);
}

export const getAnalyserById = (id) => analysers[id];
