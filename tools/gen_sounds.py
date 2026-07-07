#!/usr/bin/env python3
"""Generate the game's sound effects (48 kHz 16-bit mono WAV).

Run from the project root:  python3 tools/gen_sounds.py
Replace the generated files with your own WAVs any time; the game just
needs PCM 16-bit mono at 48 kHz.
"""
import math
import os
import struct
import wave

RATE = 48000


def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(
            struct.pack("<h", max(-32767, min(32767, int(s * 32767))))
            for s in samples))
    print("wrote", path)


def jump():
    """Short rising blip."""
    n = int(RATE * 0.18)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / n
        f = 280 + 400 * t
        phase += 2 * math.pi * f / RATE
        env = min(1.0, i / (RATE * 0.005)) * (1 - t) ** 1.5
        out.append(0.55 * env * (math.sin(phase) + 0.3 * math.sin(2 * phase)))
    return out


def fall():
    """Descending whistle into a thump."""
    out = []
    n1 = int(RATE * 0.28)
    phase = 0.0
    for i in range(n1):
        t = i / n1
        f = 520 - 380 * t
        phase += 2 * math.pi * f / RATE
        env = min(1.0, i / (RATE * 0.005)) * (1 - t)
        out.append(0.45 * env * math.sin(phase))
    n2 = int(RATE * 0.12)
    phase = 0.0
    for i in range(n2):
        t = i / n2
        f = 90 - 40 * t
        phase += 2 * math.pi * f / RATE
        out.append(0.9 * (1 - t) ** 2 * math.sin(phase))
    return out


os.makedirs("assets", exist_ok=True)
write("assets/jump.wav", jump())
write("assets/fall.wav", fall())
