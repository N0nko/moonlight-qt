#!/usr/bin/env python3
"""Offline generic, gain-bounded speaker filters. Requires NumPy, not at runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import wave
import numpy as np

RATE, FFT, LENGTH, DELAY = 48000, 4096, 512, 48


def design(data):
    f = np.fft.rfftfreq(FFT, 1 / RATE)
    phase = np.exp(-2j * np.pi * f * DELAY / RATE)

    def hrtf(angle):
        h = data['filters'][str(round(angle))]
        return np.array([np.fft.rfft(h[ear], FFT) *
                         np.exp(-2j * np.pi * f * h['delay_' + ear])
                         for ear in ('left', 'right')])

    # Symmetric sum/difference design preserves centred/mono content exactly.
    # Fit multiple distances rather than inverting one narrow listening point.
    distances = [.35, .45, .60, .75]
    ratios = []
    for distance in distances:
        h = hrtf(np.degrees(np.arctan(.11 / distance)))
        near = np.hypot(distance, .11 - .0875)
        far = np.hypot(distance, .11 + .0875)
        ratio = h[1] * h[0].conj() / (np.abs(h[0]) ** 2 + 1e-6)
        ratios.append(ratio * near / far)
    ratios = np.array(ratios)
    difference, target = 1 - ratios, 1 + ratios
    g = np.sum(difference.conj() * target, axis=0) / (
        np.sum(np.abs(difference) ** 2, axis=0) + .12)
    g *= np.minimum(1, 1.8 / np.maximum(np.abs(g), 1e-9))
    band = (1 / (1 + (350 / np.maximum(f, 1)) ** 6) /
            (1 + (f / 3000) ** 8))
    g = 1 + .65 * band * (g - 1)
    impulse = np.zeros(LENGTH)
    impulse[DELAY] = 1
    diff = np.fft.irfft(g * phase, FFT)[:LENGTH]
    diff[-64:] *= np.linspace(1, 0, 64)
    direct, cross = (impulse + diff) / 2, (impulse - diff) / 2
    stereo = np.array([[direct, cross], [cross, direct]])

    # Remove the common frontal measurement response with a bounded inverse;
    # preserve the directional interaural differences, not the source speaker EQ.
    front = hrtf(0).mean(axis=0)
    inverse = front.conj() / (np.abs(front) ** 2 + .03)
    inverse *= np.minimum(1, 2 / np.maximum(np.abs(inverse), 1e-9))
    virtual = []
    for angle in (35, -35, 0, None, 150, -150, 100, -100):
        if angle is None:
            # LFE is kept modest; do not ask small speakers to create sub-bass.
            h = np.tile(.25 / (1 + 1j * f / 120), (2, 1))
        else:
            h = hrtf(angle) * inverse
        kernel = np.fft.irfft(h * phase, FFT, axis=1)[:, :LENGTH]
        kernel[:, -64:] *= np.linspace(1, 0, 64)
        virtual.append(kernel)
    surround = np.zeros((2, 8, 2047))
    for channel, h in enumerate(virtual):
        surround[0, channel, :1023] = np.convolve(direct, h[0]) + np.convolve(cross, h[1])
        surround[1, channel, :1023] = np.convolve(cross, h[0]) + np.convolve(direct, h[1])
        # Normalize directional energy together, preserving interaural differences.
        # Never boost the deliberately quiet LFE channel.
        energy = np.sqrt(np.sum(surround[:, channel] ** 2))
        surround[:, channel] *= min(1, 1 / max(energy, 1e-9))
        if channel in (4, 5):
            # Quiet lateral early reflections supply a rear externalization cue.
            # No feedback/reverb tail, and no change to the direct arrival time.
            side = 1 if channel == 4 else -1
            for angle, delay, level in ((-60 * side, 576, .22), (65 * side, 960, .14)):
                response = hrtf(angle) * inverse / (1 + 1j * f / 5000)
                reflected = np.fft.irfft(response * phase, FFT, axis=1)[:, :LENGTH]
                reflected[:, -64:] *= np.linspace(1, 0, 64)
                pair = np.array([
                    np.convolve(direct, reflected[0]) + np.convolve(cross, reflected[1]),
                    np.convolve(cross, reflected[0]) + np.convolve(direct, reflected[1])])
                pair *= level / max(1, np.sqrt(np.sum(pair ** 2)))
                surround[:, channel, delay:delay + 1023] += pair
    return stereo, surround, ratios, f


def write_matrix(path, matrix, headroom_db):
    # Normal headroom, with the stereo-linked runtime limiter covering overloads.
    # A worst-case FIR L1 bound would unnecessarily attenuate normal surround 34 dB.
    gain = 10 ** (-headroom_db / 20)
    matrix = matrix * gain
    assert np.max(np.abs(matrix)) < 1
    interleaved = matrix.reshape(-1, matrix.shape[-1]).T
    pcm = np.rint(interleaved * 2147483647).astype('<i4')
    with wave.open(str(path), 'wb') as wav:
        wav.setparams((interleaved.shape[1], 4, RATE, len(pcm), 'NONE', 'not compressed'))
        wav.writeframes(pcm.tobytes())
    return matrix, gain


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('hrtf', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = json.loads(args.hrtf.read_text())
    assert data['sample_rate'] == RATE
    stereo, surround, ratios, f = design(data)
    args.output.mkdir(parents=True, exist_ok=True)
    stereo, sg = write_matrix(args.output / 'stereo.wav', stereo, 4.5)
    surround, vg = write_matrix(args.output / 'surround.wav', surround, 9)
    assert np.all(np.isfinite(stereo)) and np.all(np.isfinite(surround))
    mono = stereo.sum(axis=1)
    expected = np.zeros(LENGTH)
    expected[DELAY] = sg
    assert np.max(np.abs(mono - expected)) < 1e-12
    selected = (f >= 500) & (f <= 2500)
    G = np.fft.rfft(stereo / sg, FFT, axis=-1)
    metrics = []
    for d, ratio in zip([.35, .45, .60, .75], ratios):
        before = np.abs(ratio)
        after = np.abs((ratio * G[0, 0] + G[1, 0]) /
                       (G[0, 0] + ratio * G[1, 0] + 1e-12))
        metrics.append({'distance_m': d, 'median_crosstalk_reduction_db_500_2500':
                        float(np.median(20 * np.log10((before[selected] + 1e-9) /
                                                      (after[selected] + 1e-9))))})
    report = {'sample_rate': RATE, 'generic_model_only': True,
              'hrtf_sha256': data['sha256'], 'speaker_spacing_m_assumed': .22,
              'stereo_gain_db': float(20 * np.log10(sg)),
              'surround_gain_db': float(20 * np.log10(vg)),
              'stereo_design_delay_ms': DELAY / RATE * 1000,
              'surround_design_delay_ms': 2 * DELAY / RATE * 1000,
              'rear_only_reflections': [{'delay_ms': 12, 'gain': .22},
                                        {'delay_ms': 20, 'gain': .14}],
              'model_results_not_acoustic_measurements': metrics,
              'assets': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in args.output.glob('*.wav')}}
    (args.output / 'design.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
