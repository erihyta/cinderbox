"""Generates the placeholder sound effects, the audio counterpart of the procedural rig.

They are deliberately plain: short, mono, 22050 Hz, a few KB each, meant to be replaced by a mod or
by real assets later.
"""
import math
import os
import random
import struct

RATE = 22050


def write_wav(path, samples):
    frames = bytearray()
    for s in samples:
        v = max(-1.0, min(1.0, s))
        frames += struct.pack("<h", int(v * 32000))
    data = bytes(frames)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
    header += struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(header + data)
    print("%-28s %5d bytes" % (os.path.basename(path), len(data) + 44))


def env(i, n, attack=0.01, power=3.0):
    t = i / n
    a = min(1.0, t / attack) if attack > 0 else 1.0
    return a * (1.0 - t) ** power


def pop(n=int(RATE * 0.12)):
    out = []
    phase = 0.0
    for i in range(n):
        freq = 900.0 * math.exp(-4.0 * i / n) + 220.0
        phase += 2 * math.pi * freq / RATE
        out.append(math.sin(phase) * env(i, n, 0.005, 2.5) * 0.7)
    return out


def thud(n=int(RATE * 0.22)):
    rng = random.Random(7)
    out = []
    p1 = p2 = 0.0
    for i in range(n):
        p1 += 2 * math.pi * 120.0 / RATE
        p2 += 2 * math.pi * 74.0 / RATE
        tone = 0.6 * math.sin(p1) + 0.5 * math.sin(p2)
        noise = (rng.random() * 2 - 1) * 0.25 * env(i, n, 0.002, 8.0)
        out.append((tone * env(i, n, 0.003, 4.0) + noise) * 0.8)
    return out


def whoosh(n=int(RATE * 0.18)):
    rng = random.Random(11)
    out = []
    prev = 0.0
    for i in range(n):
        raw = rng.random() * 2 - 1
        # A one-pole low pass that opens up over time, which reads as a swish.
        alpha = 0.06 + 0.5 * (i / n)
        prev = prev + alpha * (raw - prev)
        shape = math.sin(math.pi * i / n) ** 2
        out.append(prev * shape * 0.6)
    return out


def shatter(n=int(RATE * 0.26)):
    rng = random.Random(23)
    out = []
    prev = 0.0
    p = 0.0
    for i in range(n):
        raw = rng.random() * 2 - 1
        prev = prev + 0.7 * (raw - prev)
        p += 2 * math.pi * (300.0 * math.exp(-6.0 * i / n) + 90.0) / RATE
        out.append((prev * 0.8 + math.sin(p) * 0.4) * env(i, n, 0.001, 5.0) * 0.75)
    return out


def step(n=int(RATE * 0.10)):
    rng = random.Random(41)
    out = []
    prev = 0.0
    p = 0.0
    for i in range(n):
        prev = prev + 0.35 * ((rng.random() * 2 - 1) - prev)
        p += 2 * math.pi * (190.0 * math.exp(-8.0 * i / n) + 60.0) / RATE
        out.append((prev * 0.55 + math.sin(p) * 0.45) * env(i, n, 0.002, 6.0) * 0.5)
    return out


def knock(n=int(RATE * 0.14)):
    out = []
    p1 = p2 = 0.0
    for i in range(n):
        p1 += 2 * math.pi * (330.0 * math.exp(-5.0 * i / n) + 110.0) / RATE
        p2 += 2 * math.pi * (155.0 * math.exp(-5.0 * i / n) + 55.0) / RATE
        out.append((math.sin(p1) * 0.5 + math.sin(p2) * 0.6) * env(i, n, 0.001, 5.0) * 0.7)
    return out


def blip(n=int(RATE * 0.09)):
    out = []
    phase = 0.0
    for i in range(n):
        freq = 520.0 + 700.0 * (i / n)
        phase += 2 * math.pi * freq / RATE
        # Square-ish, so it reads as synthetic next to the others.
        out.append((1.0 if math.sin(phase) > 0 else -1.0) * env(i, n, 0.01, 3.0) * 0.35)
    return out


out_dir = "godot/assets/sfx"
os.makedirs(out_dir, exist_ok=True)
write_wav(os.path.join(out_dir, "prop_spawn.wav"), pop())
write_wav(os.path.join(out_dir, "prop_destroy.wav"), shatter())
write_wav(os.path.join(out_dir, "jump.wav"), whoosh())
write_wav(os.path.join(out_dir, "land.wav"), thud())
write_wav(os.path.join(out_dir, "footstep.wav"), step())
write_wav(os.path.join(out_dir, "impact.wav"), knock())

mod_dir = "mods_src/example_neon/assets/sfx"
os.makedirs(mod_dir, exist_ok=True)
write_wav(os.path.join(mod_dir, "neon_blip.wav"), blip())
