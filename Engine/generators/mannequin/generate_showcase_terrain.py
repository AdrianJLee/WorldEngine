#!/usr/bin/env python3
"""Generate the AnimationShowcase terrain (.wterrain v1, binary, little endian)."""
import math, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(REPO, "templates", "project-example", "assets", "terrains", "animation_showcase.wterrain")

W = H = 65
CELL = 1.0
HEIGHT_SCALE = 1.0

def smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)

def height(x, z):
    ramp = smoothstep(30.0, 44.0, x) * 2.35
    plateau_roll = smoothstep(44.0, 64.0, x) * (0.10 * math.sin(z * 0.20))
    basin = -0.42 * math.exp(-(((x - 12.0) ** 2) + ((z - 40.0) ** 2)) / 60.0)
    ripple = 0.055 * math.sin(x * 0.23) * math.cos(z * 0.19)
    return ramp + plateau_roll + basin + ripple

heights = []
for z in range(H):
    for x in range(W):
        heights.append(height(float(x), float(z)))

splat = []
for z in range(H):
    for x in range(W):
        hx = height(x + 1.0, z) - height(x - 1.0, z) if 0 < x < W - 1 else 0.0
        hz = height(x, z + 1.0) - height(x, z - 1.0) if 0 < z < H - 1 else 0.0
        slope = min(1.0, math.sqrt(hx * hx + hz * hz) * 0.55)
        high = smoothstep(1.0, 2.4, height(float(x), float(z)))
        grass = max(0.0, 1.0 - slope * 0.75 - high * 0.55)
        dirt = min(1.0, slope * 0.85 + 0.05)
        rock = min(1.0, high * 0.80 + slope * 0.15)
        sand = max(0.0, 0.08 - high * 0.08)
        total = grass + dirt + rock + sand + 1e-6
        grass, dirt, rock, sand = grass / total, dirt / total, rock / total, sand / total
        splat.append(int(round(grass * 255)) | (int(round(dirt * 255)) << 8) |
                     (int(round(rock * 255)) << 16) | (int(round(sand * 255)) << 24))

layers = [
    ("Grass", "textures/terrain/grass_albedo.png", "textures/terrain/grass_normal.png", 0.82, 0.0, (10.0, 10.0)),
    ("Dirt",  "textures/terrain/dirt_albedo.png",  "textures/terrain/dirt_normal.png",  0.90, 0.0, (10.0, 10.0)),
    ("Rock",  "textures/terrain/rock_albedo.png",  "textures/terrain/rock_normal.png",  0.62, 0.0, (5.0, 5.0)),
    ("Sand",  "textures/terrain/sand_albedo.png",  "textures/terrain/sand_normal.png",  0.90, 0.0, (10.0, 10.0)),
]

def wstr(f, s):
    raw = s.encode("utf-8")
    f.write(struct.pack("<I", len(raw)))
    f.write(raw)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as f:
    f.write(struct.pack("<IIIIff", 0x57544552, 1, W, H, CELL, HEIGHT_SCALE))
    for name, alb, nor, rough, metal, tiling in layers:
        wstr(f, name)
        wstr(f, alb)
        f.write(struct.pack("<Q", 0))
        wstr(f, nor)
        f.write(struct.pack("<Q", 0))
        f.write(struct.pack("<fff", rough, metal, tiling[0]))
        f.write(struct.pack("<f", tiling[1]))
    f.write(struct.pack("<%df" % len(heights), *heights))
    f.write(struct.pack("<%dI" % len(splat), *splat))
    f.write(bytes(len(heights)))

print("Wrote", OUT, os.path.getsize(OUT), "bytes")
print("height range", min(heights), max(heights))
