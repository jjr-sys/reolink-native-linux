#!/usr/bin/env python3
"""Generate the built-in Alert sounds (packaging/sounds/*.wav).

Every clip is synthesised here from sine waves and noise, so the sounds are this
project's own work with no third-party licence. Re-run after changing a recipe:

    python3 tools/gen-alert-sounds.py

44.1 kHz, mono, 16-bit, peak at -3 dBFS, short fades at both ends.
"""
import math
import os
import random
import struct
import wave

RATE = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "packaging", "sounds")


def silence(sec):
    return [0.0] * int(sec * RATE)


def tone(freq, sec, partials=((1, 1.0),), decay=0.0, attack=0.005, sweep_to=None, vibrato=None):
    """Sum of partials (ratio, gain). `decay`: exponential rate per second (0 = flat, with a
    short release). `sweep_to`: end frequency for a linear glide. `vibrato`: (Hz, depth ratio)."""
    n = int(sec * RATE)
    out = []
    phase = [0.0] * len(partials)
    for i in range(n):
        t = i / RATE
        f = freq if sweep_to is None else freq + (sweep_to - freq) * (i / n)
        if vibrato:
            f *= 1 + vibrato[1] * math.sin(2 * math.pi * vibrato[0] * t)
        s = 0.0
        for k, (ratio, gain) in enumerate(partials):
            phase[k] += 2 * math.pi * f * ratio / RATE
            s += gain * math.sin(phase[k])
        env = min(1.0, t / attack) if attack > 0 else 1.0
        env *= math.exp(-decay * t) if decay > 0 else min(1.0, (sec - t) / 0.01)
        out.append(s * env)
    return out


def noise(sec, decay):
    rnd = random.Random(7)
    return [rnd.uniform(-1, 1) * math.exp(-decay * i / RATE) for i in range(int(sec * RATE))]


def mix(*tracks):
    n = max(len(t) for t in tracks)
    return [sum(t[i] for t in tracks if i < len(t)) for i in range(n)]


def cat(*parts):
    out = []
    for p in parts:
        out.extend(p)
    return out


def square_ish(freq, sec, harmonics=7):
    return tone(freq, sec, partials=[(h, 1.0 / h) for h in range(1, harmonics + 1, 2)])


BELL = ((1, 1.0), (2.76, 0.5), (5.40, 0.25), (8.93, 0.12))
MARIMBA = ((1, 1.0), (3.93, 0.3), (9.2, 0.08))
CHIME = ((1, 1.0), (2, 0.35), (3, 0.12))


def bird_chirp():
    def chirp(f0, f1, sec):
        return tone(f0, sec, sweep_to=f1, vibrato=(60, 0.04), attack=0.004)
    return cat(chirp(3200, 4800, 0.07), silence(0.04), chirp(3400, 5200, 0.07), silence(0.04),
               chirp(4800, 3000, 0.12), silence(0.08), chirp(3600, 5000, 0.06))


SOUNDS = {
    "beep": lambda: tone(1000, 0.18),
    "double-beep": lambda: cat(tone(1000, 0.12), silence(0.08), tone(1000, 0.12)),
    "triple-beep": lambda: cat(tone(1200, 0.1), silence(0.06), tone(1200, 0.1), silence(0.06),
                               tone(1200, 0.1)),
    "ping": lambda: tone(1760, 0.7, partials=((1, 1.0), (2.01, 0.15)), decay=6),
    "chime": lambda: mix(tone(1047, 1.2, CHIME, decay=3.5),
                         silence(0.16) + tone(1319, 1.04, CHIME, decay=3.5),
                         silence(0.32) + tone(1568, 0.9, CHIME, decay=3.5)),
    "bell": lambda: tone(620, 1.6, BELL, decay=2.8, attack=0.002),
    "marimba": lambda: mix(tone(523, 0.5, MARIMBA, decay=9),
                           silence(0.12) + tone(659, 0.5, MARIMBA, decay=9),
                           silence(0.24) + tone(784, 0.6, MARIMBA, decay=8)),
    "bird-chirp": bird_chirp,
    "horn": lambda: mix(square_ish(415, 0.55), square_ish(523, 0.55)),
    "knock": lambda: cat(*[cat(mix(tone(170, 0.09, decay=45, attack=0.001), [x * 0.5 for x in noise(0.03, 120)]),
                               silence(0.13)) for _ in range(3)]),
    "whistle": lambda: cat(tone(1400, 0.22, sweep_to=2600, attack=0.02), silence(0.08),
                           tone(2400, 0.45, sweep_to=1300, attack=0.02, vibrato=(7, 0.01))),
    "pop": lambda: tone(700, 0.07, sweep_to=140, decay=30, attack=0.001),
    "short-siren": lambda: cat(tone(650, 0.3, sweep_to=1250, partials=CHIME),
                               tone(1250, 0.3, sweep_to=650, partials=CHIME),
                               tone(650, 0.3, sweep_to=1250, partials=CHIME),
                               tone(1250, 0.3, sweep_to=650, partials=CHIME)),
    "buzzer": lambda: square_ish(150, 0.6, harmonics=15),
}


def write(name, samples):
    fade = int(0.003 * RATE)
    for i in range(min(fade, len(samples))):
        samples[i] *= i / fade
        samples[-1 - i] *= i / fade
    peak = max(abs(s) for s in samples) or 1.0
    gain = 10 ** (-3 / 20) / peak
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(s * gain * 32767)) for s in samples))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, make in SOUNDS.items():
        write(name, make())
        print(name)
