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


def gunshot(n=int(RATE * 0.35)):
    rng = random.Random(53)
    out = []
    prev = 0.0
    p = 0.0
    for i in range(n):
        raw = rng.random() * 2 - 1
        # A bright crack that darkens into a short tail, over a low body.
        alpha = 0.9 * math.exp(-10.0 * i / n) + 0.05
        prev = prev + alpha * (raw - prev)
        p += 2 * math.pi * (140.0 * math.exp(-7.0 * i / n) + 45.0) / RATE
        out.append((prev * 0.85 + math.sin(p) * 0.5) * env(i, n, 0.0005, 6.0) * 0.9)
    return out


def dry_click(n=int(RATE * 0.05)):
    rng = random.Random(59)
    out = []
    for i in range(n):
        out.append((rng.random() * 2 - 1) * env(i, n, 0.0005, 12.0) * 0.5)
    return out


def reload_clack(n=int(RATE * 0.45)):
    # Two mechanical clicks: magazine out, magazine in.
    rng = random.Random(61)
    out = []
    for i in range(n):
        t = i / n
        v = 0.0
        for at in (0.05, 0.62):
            if t >= at:
                k = (t - at) * n
                v += (rng.random() * 2 - 1) * math.exp(-k / (RATE * 0.012)) * 0.6
                v += math.sin(2 * math.pi * 900.0 * k / RATE) * math.exp(-k / (RATE * 0.02)) * 0.25
        out.append(v)
    return out


def hit_marker(n=int(RATE * 0.08)):
    out = []
    phase = 0.0
    for i in range(n):
        phase += 2 * math.pi * 1800.0 / RATE
        out.append(math.sin(phase) * env(i, n, 0.002, 4.0) * 0.35)
    return out


def body_hit(n=int(RATE * 0.16)):
    rng = random.Random(67)
    out = []
    prev = 0.0
    p = 0.0
    for i in range(n):
        prev = prev + 0.25 * ((rng.random() * 2 - 1) - prev)
        p += 2 * math.pi * (95.0 * math.exp(-4.0 * i / n) + 50.0) / RATE
        out.append((prev * 0.6 + math.sin(p) * 0.7) * env(i, n, 0.001, 5.0) * 0.8)
    return out


def ricochet(n=int(RATE * 0.25)):
    out = []
    phase = 0.0
    for i in range(n):
        freq = 2400.0 * math.exp(-2.5 * i / n) + 600.0
        phase += 2 * math.pi * freq / RATE
        out.append(math.sin(phase) * env(i, n, 0.001, 3.0) * 0.3)
    return out


out_dir = "godot/assets/sfx"
os.makedirs(out_dir, exist_ok=True)
write_wav(os.path.join(out_dir, "prop_spawn.wav"), pop())
write_wav(os.path.join(out_dir, "prop_destroy.wav"), shatter())
write_wav(os.path.join(out_dir, "jump.wav"), whoosh())
write_wav(os.path.join(out_dir, "land.wav"), thud())
write_wav(os.path.join(out_dir, "footstep.wav"), step())
write_wav(os.path.join(out_dir, "impact.wav"), knock())

def chime(notes, note_seconds=0.14):
    out = []
    for freq in notes:
        n = int(RATE * note_seconds)
        phase = 0.0
        for i in range(n):
            phase += 2 * math.pi * freq / RATE
            out.append((math.sin(phase) * 0.6 + math.sin(2 * phase) * 0.2) * env(i, n, 0.01, 2.0) * 0.6)
    return out


# The deathmatch mod's sounds belong to its workshop item.
deathmatch_dir = "server_mods/deathmatch/client/assets/sfx"
os.makedirs(deathmatch_dir, exist_ok=True)
write_wav(os.path.join(deathmatch_dir, "round_end.wav"), chime([523.0, 659.0, 784.0, 1047.0], 0.16))
write_wav(os.path.join(deathmatch_dir, "round_start.wav"), chime([392.0, 392.0, 784.0], 0.12))

# The pistol's sounds belong to its workshop item, not the base game.
pistol_dir = "server_mods/pistol/client/assets/sfx"
os.makedirs(pistol_dir, exist_ok=True)
write_wav(os.path.join(pistol_dir, "gunshot.wav"), gunshot())
write_wav(os.path.join(pistol_dir, "dry_click.wav"), dry_click())
write_wav(os.path.join(pistol_dir, "reload.wav"), reload_clack())
write_wav(os.path.join(pistol_dir, "hit_marker.wav"), hit_marker())
write_wav(os.path.join(pistol_dir, "body_hit.wav"), body_hit())
write_wav(os.path.join(pistol_dir, "ricochet.wav"), ricochet())

# The melee mod's sounds belong to its workshop item.
melee_dir = "server_mods/melee/client/assets/sfx"
os.makedirs(melee_dir, exist_ok=True)
write_wav(os.path.join(melee_dir, "swing.wav"), whoosh(int(RATE * 0.28)))
write_wav(os.path.join(melee_dir, "bat_hit.wav"), thud(int(RATE * 0.18)))

# The robot character's own sound (its bat swing's companion track).
robot_dir = "characters/robot/client/characters/robot"
os.makedirs(robot_dir, exist_ok=True)
write_wav(os.path.join(robot_dir, "whoosh.wav"), whoosh(int(RATE * 0.32)))

mod_dir = "mods_src/example_neon/assets/sfx"
os.makedirs(mod_dir, exist_ok=True)
write_wav(os.path.join(mod_dir, "neon_blip.wav"), blip())
