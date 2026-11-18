#!/usr/bin/env python3
"""Compares two renders of the same Strudel code: the real superdough (reference, node-web-audio-api)
and superdough C++ (concha_render). Prints the measures; used by run_cases.py.

Measures (on the part where the reference is above -50 dBFS):
  env   : 10 ms RMS envelope, median / 95th percentile of |difference| in dB
  spec  : average spectrum in third-octave bands (40 Hz - 16 kHz), largest |difference| in dB
          among the bands within 40 dB of the loudest
  bal   : left/right balance difference in dB
  len   : end of the sound (last 10 ms frame above -60 dBFS), difference in ms
  corr  : correlation of the waveforms (1 = identical; only meaningful without noise)
"""
import sys
import numpy as np


def read_wav(path):
    d = open(path, 'rb').read()
    # minimal reader for 32-bit float / 16-bit PCM WAV
    pos, fmt, ch, data = 12, 3, 2, None
    while pos < len(d) - 8:
        cid, size = d[pos:pos + 4], int.from_bytes(d[pos + 4:pos + 8], 'little')
        if cid == b'fmt ':
            fmt = int.from_bytes(d[pos + 8:pos + 10], 'little')
            ch = int.from_bytes(d[pos + 10:pos + 12], 'little')
        elif cid == b'data':
            data = d[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    x = np.frombuffer(data, dtype=np.float32 if fmt == 3 else np.int16).astype(np.float64)
    if fmt != 3:
        x /= 32768.0
    return x.reshape(-1, ch)


def db(x):
    return 20 * np.log10(np.maximum(x, 1e-10))


def envelope(x, n=480):
    m = x.mean(axis=1)
    k = len(m) // n
    return np.sqrt((m[:k * n].reshape(k, n) ** 2).mean(axis=1))


def bands(x, sr=48000):
    m = x.mean(axis=1)
    win = 4096
    if len(m) < win:
        m = np.pad(m, (0, win - len(m)))
    frames = [m[i:i + win] * np.hanning(win) for i in range(0, len(m) - win + 1, win // 2)]
    spec = np.mean([np.abs(np.fft.rfft(f)) ** 2 for f in frames], axis=0)
    freqs = np.fft.rfftfreq(win, 1 / sr)
    edges = 40 * 2 ** (np.arange(0, 28) / 3)  # 40 Hz .. ~16 kHz
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = (freqs >= lo) & (freqs < hi)
        out.append(spec[sel].sum() if sel.any() else 0)
    return np.array(out)


def compare(ref_path, test_path, noise=False, spec_from=200, shift=0):
    a, b = read_wav(ref_path), read_wav(test_path)
    if shift:  # the test render is `shift` samples early (known latency difference)
        b = np.concatenate([np.zeros((shift, b.shape[1])), b])
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    # noise: two independent random draws, compared on 100 ms frames and above 200 Hz
    ea, eb = (envelope(a, 4800), envelope(b, 4800)) if noise else (envelope(a), envelope(b))
    loud = db(ea) > -50
    env_diff = np.abs(db(ea[loud]) - db(eb[loud])) if loud.any() else np.array([0.0])
    ba, bb = bands(a), bands(b)
    keep = db(np.sqrt(ba)) > db(np.sqrt(ba.max())) - 40
    if noise:
        keep &= 40 * 2 ** (np.arange(0, 27) / 3) >= spec_from
    spec_diff = np.abs(10 * np.log10(np.maximum(ba[keep], 1e-20) / np.maximum(bb[keep], 1e-20)))
    bal = lambda x: db(np.sqrt((x[:, 0] ** 2).mean())) - db(np.sqrt((x[:, 1] ** 2).mean()))
    def end(e):
        idx = np.nonzero(db(e) > -60)[0]
        return (idx[-1] + 1) * (100 if noise else 10) if len(idx) else 0
    ma, mb = a.mean(axis=1), b.mean(axis=1)
    corr = float(np.dot(ma, mb) / max(1e-12, np.sqrt(np.dot(ma, ma) * np.dot(mb, mb))))
    return {
        'env_median': float(np.median(env_diff)),
        'env_p95': float(np.percentile(env_diff, 95)),
        'spec_max': float(spec_diff.max()) if len(spec_diff) else 0.0,
        'bal': float(abs(bal(a) - bal(b))),
        'len_ms': float(abs(end(ea) - end(eb))),
        'corr': corr,
        'ref_peak': float(np.abs(a).max()),
        'test_peak': float(np.abs(b).max()),
    }


if __name__ == '__main__':
    r = compare(sys.argv[1], sys.argv[2])
    print(' '.join(f'{k}={v:.3f}' for k, v in r.items()))
