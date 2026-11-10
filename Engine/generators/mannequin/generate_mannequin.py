#!/usr/bin/env python3
"""Generate the WorldEngine Mannequin: a rigged, animated humanoid glTF 2.0 asset.

Design goals
------------
* Standard humanoid rig (22 bones, T-pose, Y-up, faces +Z, feet at y=0).
* Segmented "training mannequin" look: graphite body, ivory armour plates,
  cyan emissive visor/accents.  No external textures -> the asset is portable.
* Rigid skin weights (one bone per vertex) so every pose stays artifact free.
* Nine clips: Idle, Walk, Run, WalkBack, StrafeLeft, StrafeRight, Jump, Fall, Aim.
* Pure stdlib (no numpy / no external tools), deterministic output.

Run:  python Engine/generators/mannequin/generate_mannequin.py
Out:  Engine/content/models/characters/mannequin.gltf
"""

import base64
import json
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(REPO, "Engine", "content", "models", "characters", "mannequin.gltf")

TAU = math.pi * 2.0
FPS = 30.0

# ---------------------------------------------------------------------------
# tiny vector / quaternion helpers
# ---------------------------------------------------------------------------

def vadd(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def vsub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def vmul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def vdot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def vcross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])

def vlen(a): return math.sqrt(max(0.0, vdot(a, a)))

def vnorm(a):
    l = vlen(a)
    return (a[0] / l, a[1] / l, a[2] / l) if l > 1e-12 else (0.0, 1.0, 0.0)

def qaxis(axis, deg):
    h = math.radians(deg) * 0.5
    s = math.sin(h)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h))

def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)

def qnorm(q):
    l = math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
    if l < 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return (q[0] / l, q[1] / l, q[2] / l, q[3] / l)

def qslerp(a, b, t):
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
    if d < 0.0:
        b = (-b[0], -b[1], -b[2], -b[3]); d = -d
    if d > 0.9995:
        return qnorm((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                      a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t))
    th0 = math.acos(max(-1.0, min(1.0, d)))
    th = th0 * t
    s0 = math.sin(th0 - th) / math.sin(th0)
    s1 = math.sin(th) / math.sin(th0)
    return qnorm((a[0] * s0 + b[0] * s1, a[1] * s0 + b[1] * s1,
                  a[2] * s0 + b[2] * s1, a[3] * s0 + b[3] * s1))

def smoothstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)

# ---------------------------------------------------------------------------
# skeleton (world-space bind pose)
# ---------------------------------------------------------------------------

BONES = []          # ordered list of (name, parent)
BIND = {}           # name -> world position

def bone(name, parent, pos):
    BONES.append((name, parent))
    BIND[name] = pos

bone("Root", None, (0.0, 0.0, 0.0))
bone("Hips", "Root", (0.0, 0.98, 0.0))
bone("Spine", "Hips", (0.0, 1.10, 0.0))
bone("Chest", "Spine", (0.0, 1.30, 0.0))
bone("Neck", "Chest", (0.0, 1.54, 0.0))
bone("Head", "Neck", (0.0, 1.62, 0.0))
for side, s in (("L", 1.0), ("R", -1.0)):
    bone("Shoulder." + side, "Chest", (s * 0.10, 1.47, 0.0))
    bone("UpperArm." + side, "Shoulder." + side, (s * 0.22, 1.47, 0.0))
    bone("LowerArm." + side, "UpperArm." + side, (s * 0.47, 1.47, 0.0))
    bone("Hand." + side, "LowerArm." + side, (s * 0.71, 1.47, 0.0))
    bone("Thigh." + side, "Hips", (s * 0.105, 0.94, 0.0))
    bone("Shin." + side, "Thigh." + side, (s * 0.105, 0.53, 0.0))
    bone("Foot." + side, "Shin." + side, (s * 0.105, 0.105, 0.0))
    bone("Toe." + side, "Foot." + side, (s * 0.105, 0.035, 0.16))

BONE_INDEX = {name: i for i, (name, _) in enumerate(BONES)}

# ---------------------------------------------------------------------------
# mesh builder (one primitive per material)
# ---------------------------------------------------------------------------

MATERIALS = [
    ("Mannequin Body",   (0.018, 0.021, 0.028, 1.0), 0.22, 0.52, (0.0, 0.0, 0.0)),
    ("Mannequin Armor",  (0.72, 0.72, 0.68, 1.0), 0.08, 0.34, (0.0, 0.0, 0.0)),
    ("Mannequin Joint",  (0.012, 0.013, 0.016, 1.0), 0.12, 0.80, (0.0, 0.0, 0.0)),
    ("Mannequin Accent", (0.008, 0.045, 0.060, 1.0), 0.0, 0.28, (0.10, 0.90, 1.0)),
    ("Mannequin Visor",  (0.004, 0.008, 0.012, 1.0), 0.85, 0.12, (0.02, 0.26, 0.34)),
]
MAT_INDEX = {m[0]: i for i, m in enumerate(MATERIALS)}

class Builder:
    def __init__(self):
        self.pos = {i: [] for i in MAT_INDEX.values()}
        self.nrm = {i: [] for i in MAT_INDEX.values()}
        self.joints = {i: [] for i in MAT_INDEX.values()}
        self.weights = {i: [] for i in MAT_INDEX.values()}
        self.idx = {i: [] for i in MAT_INDEX.values()}

    def add(self, material, verts, normals, indices, bone_name):
        mi = MAT_INDEX[material]
        base = len(self.pos[mi])
        j = BONE_INDEX[bone_name]
        for i, p in enumerate(verts):
            self.pos[mi].append(p)
            self.nrm[mi].append(vnorm(normals[i]))
            self.joints[mi].append((j, 0, 0, 0))
            self.weights[mi].append((1.0, 0.0, 0.0, 0.0))
        # winding correction against the analytic vertex normals
        for t in range(0, len(indices) - 2, 3):
            a, b, c = indices[t], indices[t + 1], indices[t + 2]
            if a == b or b == c or a == c:
                continue
            pa, pb, pc = verts[a], verts[b], verts[c]
            n = vcross(vsub(pb, pa), vsub(pc, pa))
            ref = vadd(vadd(normals[a], normals[b]), normals[c])
            if vdot(n, ref) < 0.0:
                b, c = c, b
            self.idx[mi].extend((base + a, base + b, base + c))

B = Builder()

# ---------------------------------------------------------------------------
# primitive generators
# ---------------------------------------------------------------------------

def sgnpow(x, e):
    if x == 0.0:
        return 0.0
    return math.copysign(abs(x) ** e, x)

def superellipsoid(center, half, e1, e2, bone_name, material, seg=28, rings=18):
    cx, cy, cz = center
    hx, hy, hz = half
    verts, normals, indices = [], [], []

    def surf(u, v):
        cv, sv = math.cos(v), math.sin(v)
        cu, su = math.cos(u), math.sin(u)
        return (cx + hx * sgnpow(cv, e1) * sgnpow(cu, e2),
                cy + hy * sgnpow(sv, e1),
                cz + hz * sgnpow(cv, e1) * sgnpow(su, e2))

    grid = []
    for i in range(rings + 1):
        v = -math.pi * 0.5 + math.pi * i / rings
        row = []
        for j in range(seg + 1):
            u = TAU * j / seg
            p = surf(u, v)
            eps = 1e-3
            du = vsub(surf(u + eps, v), surf(u - eps, v))
            dv = vsub(surf(u, v + eps), surf(u, v - eps))
            n = vcross(du, dv)
            if vlen(n) < 1e-9:
                n = vsub(p, center)
            if vdot(n, vsub(p, center)) < 0.0:
                n = vmul(n, -1.0)
            row.append((p, vnorm(n)))
        grid.append(row)

    for i in range(rings + 1):
        for j in range(seg + 1):
            p, n = grid[i][j]
            verts.append(p); normals.append(n)
    for i in range(rings):
        for j in range(seg):
            a = i * (seg + 1) + j
            b = a + 1
            c = a + (seg + 1) + 1
            d = a + (seg + 1)
            indices.extend((a, b, c, a, c, d))
    B.add(material, verts, normals, indices, bone_name)

def capsule(p0, p1, radius, bone_name, material, seg=20, cap_rings=6):
    axis = vsub(p1, p0)
    h = vlen(axis)
    if h < 1e-6:
        superellipsoid(p0, (radius, radius, radius), 1.0, 1.0, bone_name, material, seg, seg // 2)
        return
    ay = vmul(axis, 1.0 / h)
    tmp = (1.0, 0.0, 0.0) if abs(ay[0]) < 0.9 else (0.0, 0.0, 1.0)
    ax = vnorm(vcross(ay, tmp))
    az = vcross(ay, ax)
    center = vmul(vadd(p0, p1), 0.5)
    half = h * 0.5

    rows = []
    # bottom cap (-90 .. 0)
    for i in range(cap_rings + 1):
        a = math.radians(-90.0 + 90.0 * i / cap_rings)
        y = -half + radius * math.sin(a)
        r = radius * math.cos(a)
        rows.append((y, r, math.sin(a)))
    # top cap (0 .. 90); skip duplicate ring at 0
    for i in range(1, cap_rings + 1):
        a = math.radians(90.0 * i / cap_rings)
        y = half + radius * math.sin(a)
        r = radius * math.cos(a)
        rows.append((y, r, math.sin(a)))

    verts, normals, indices = [], [], []
    for y, r, sa in rows:
        for j in range(seg + 1):
            u = TAU * j / seg
            radial = vadd(vmul(ax, math.cos(u)), vmul(az, math.sin(u)))
            p = vadd(center, vadd(vmul(ay, y), vmul(radial, r)))
            n = vnorm(vadd(vmul(radial, math.cos(math.asin(max(-1.0, min(1.0, sa))))),
                           vmul(ay, sa)))
            verts.append(p); normals.append(n)
    for i in range(len(rows) - 1):
        for j in range(seg):
            a = i * (seg + 1) + j
            b = a + 1
            c = a + (seg + 1) + 1
            d = a + (seg + 1)
            indices.extend((a, b, c, a, c, d))
    B.add(material, verts, normals, indices, bone_name)

def ring(p0, p1, radius, bone_name, material, seg=24):
    """Short open cylinder used for armour bands / collars."""
    capsule(p0, p1, radius, bone_name, material, seg=seg, cap_rings=2)

def bar(center, half, bone_name, material, e=0.45):
    superellipsoid(center, half, e, e, bone_name, material, seg=20, rings=12)

# ---------------------------------------------------------------------------
# body parts  (segmented training mannequin: graphite under-suit + ivory plates)
# ---------------------------------------------------------------------------

# pelvis / abdomen
superellipsoid((0.0, 1.005, 0.0), (0.150, 0.072, 0.096), 0.32, 0.32, "Hips", "Mannequin Body")
bar((0.0, 1.062, 0.0), (0.163, 0.030, 0.108), "Hips", "Mannequin Armor")
for s in (1.0, -1.0):
    bar((s * 0.150, 1.010, -0.005), (0.028, 0.055, 0.075), "Hips", "Mannequin Armor")
bar((0.0, 1.145, -0.002), (0.126, 0.082, 0.088), "Spine", "Mannequin Body")
for k, y in enumerate((1.095, 1.155, 1.215)):
    w = 0.108 + 0.006 * k
    bar((0.0, y, 0.082), (w, 0.020, 0.018), "Spine", "Mannequin Armor")
    bar((0.0, y - 0.006, -0.082), (w * 0.82, 0.014, 0.014), "Spine", "Mannequin Body")

# chest
superellipsoid((0.0, 1.355, 0.0), (0.185, 0.133, 0.100), 0.26, 0.26, "Chest", "Mannequin Body")
bar((0.0, 1.370, 0.096), (0.158, 0.078, 0.020), "Chest", "Mannequin Armor")
bar((0.0, 1.345, 0.119), (0.036, 0.036, 0.010), "Chest", "Mannequin Accent")
bar((0.0, 1.455, 0.075), (0.108, 0.018, 0.014), "Chest", "Mannequin Accent")
bar((0.0, 1.320, -0.104), (0.120, 0.095, 0.024), "Chest", "Mannequin Body")
for y in (1.285, 1.345, 1.405):
    bar((0.0, y, -0.126), (0.012, 0.012, 0.008), "Chest", "Mannequin Accent")
for s in (1.0, -1.0):
    bar((s * 0.105, 1.468, 0.020), (0.060, 0.024, 0.062), "Chest", "Mannequin Armor", e=0.30)

# neck / head
capsule((0.0, 1.490, 0.0), (0.0, 1.585, 0.0), 0.044, "Neck", "Mannequin Joint", seg=18, cap_rings=4)
ring((0.0, 1.500, 0.0), (0.0, 1.528, 0.0), 0.072, "Neck", "Mannequin Armor")
superellipsoid((0.0, 1.705, 0.002), (0.094, 0.116, 0.102), 0.30, 0.30, "Head", "Mannequin Armor")
superellipsoid((0.0, 1.700, 0.090), (0.080, 0.036, 0.024), 0.30, 0.40, "Head", "Mannequin Visor")
superellipsoid((0.0, 1.790, 0.006), (0.017, 0.013, 0.078), 0.35, 0.35, "Head", "Mannequin Accent")
bar((0.0, 1.612, 0.078), (0.062, 0.026, 0.036), "Head", "Mannequin Joint")
for s in (1.0, -1.0):
    superellipsoid((s * 0.094, 1.700, -0.004), (0.024, 0.040, 0.036), 0.55, 0.55,
                   "Head", "Mannequin Joint", seg=16, rings=10)
    bar((s * 0.087, 1.755, -0.028), (0.016, 0.014, 0.042), "Head", "Mannequin Armor")
    bar((s * 0.070, 1.675, 0.075), (0.022, 0.040, 0.030), "Head", "Mannequin Armor")

# arms
for side, s in (("L", 1.0), ("R", -1.0)):
    capsule((s * 0.180, 1.478, 0.0), (s * 0.292, 1.470, 0.0), 0.076,
            "Shoulder." + side, "Mannequin Armor", seg=20, cap_rings=5)
    bar((s * 0.245, 1.500, 0.0), (0.052, 0.020, 0.062), "Shoulder." + side, "Mannequin Accent", e=0.30)
    capsule((s * 0.205, 1.465, 0.0), (s * 0.455, 1.465, 0.0), 0.048,
            "UpperArm." + side, "Mannequin Body", seg=18, cap_rings=5)
    capsule((s * 0.250, 1.465, 0.0), (s * 0.365, 1.465, 0.0), 0.056,
            "UpperArm." + side, "Mannequin Armor", seg=18, cap_rings=3)
    superellipsoid((s * 0.468, 1.465, 0.0), (0.046, 0.046, 0.046), 1.0, 1.0,
                   "LowerArm." + side, "Mannequin Joint", seg=16, rings=10)
    capsule((s * 0.470, 1.465, 0.0), (s * 0.700, 1.465, 0.0), 0.043,
            "LowerArm." + side, "Mannequin Body", seg=18, cap_rings=5)
    bar((s * 0.585, 1.468, -0.012), (0.062, 0.052, 0.052), "LowerArm." + side, "Mannequin Armor", e=0.30)
    bar((s * 0.585, 1.468, 0.042), (0.050, 0.040, 0.014), "LowerArm." + side, "Mannequin Accent", e=0.30)
    superellipsoid((s * 0.718, 1.465, 0.0), (0.034, 0.034, 0.034), 1.0, 1.0,
                   "Hand." + side, "Mannequin Joint", seg=14, rings=8)
    superellipsoid((s * 0.785, 1.462, 0.0), (0.070, 0.040, 0.050), 0.30, 0.30,
                   "Hand." + side, "Mannequin Body", seg=20, rings=12)
    bar((s * 0.790, 1.482, 0.0), (0.056, 0.008, 0.042), "Hand." + side, "Mannequin Armor", e=0.30)
    bar((s * 0.815, 1.455, 0.018), (0.018, 0.024, 0.030), "Hand." + side, "Mannequin Body", e=0.40)

# legs
for side, s in (("L", 1.0), ("R", -1.0)):
    bar((s * 0.112, 0.975, 0.0), (0.062, 0.034, 0.082), "Hips", "Mannequin Armor")
    capsule((s * 0.105, 0.945, 0.0), (s * 0.105, 0.545, 0.0), 0.077,
            "Thigh." + side, "Mannequin Body", seg=20, cap_rings=5)
    bar((s * 0.105, 0.760, 0.068), (0.066, 0.140, 0.026), "Thigh." + side, "Mannequin Armor", e=0.30)
    bar((s * 0.105, 0.760, -0.068), (0.058, 0.130, 0.020), "Thigh." + side, "Mannequin Body", e=0.30)
    superellipsoid((s * 0.105, 0.532, 0.016), (0.066, 0.064, 0.070), 0.45, 0.45,
                   "Shin." + side, "Mannequin Armor", seg=18, rings=10)
    capsule((s * 0.105, 0.525, 0.0), (s * 0.105, 0.145, 0.0), 0.058,
            "Shin." + side, "Mannequin Body", seg=18, cap_rings=5)
    bar((s * 0.105, 0.335, 0.058), (0.056, 0.145, 0.024), "Shin." + side, "Mannequin Armor", e=0.30)
    bar((s * 0.105, 0.350, -0.058), (0.048, 0.110, 0.020), "Shin." + side, "Mannequin Body", e=0.30)
    superellipsoid((s * 0.105, 0.112, 0.005), (0.048, 0.048, 0.048), 1.0, 1.0,
                   "Foot." + side, "Mannequin Joint", seg=14, rings=8)
    superellipsoid((s * 0.105, 0.068, 0.032), (0.056, 0.058, 0.132), 0.28, 0.36,
                   "Foot." + side, "Mannequin Armor", seg=22, rings=12)
    superellipsoid((s * 0.105, 0.045, 0.183), (0.050, 0.040, 0.058), 0.34, 0.34,
                   "Toe." + side, "Mannequin Body", seg=18, rings=10)
    bar((s * 0.105, 0.104, -0.058), (0.044, 0.030, 0.028), "Foot." + side, "Mannequin Body", e=0.35)
# ---------------------------------------------------------------------------
# animation authoring
# ---------------------------------------------------------------------------

def arm_pose(side, lower_deg, fwd_deg, elbow_deg, wrist_deg=0.0):
    s = 1.0 if side == "L" else -1.0
    q_lower = qaxis((0, 0, 1), -s * lower_deg)
    q_swing = qaxis((1, 0, 0), -fwd_deg)
    q_upper = qmul(q_swing, q_lower)
    q_elbow = qaxis((0, 1, 0), s * elbow_deg)
    q_wrist = qaxis((0, 1, 0), s * wrist_deg)
    return q_upper, q_elbow, q_wrist

def leg_pose(side, swing_deg, knee_deg, abduct_deg=0.0, foot_deg=0.0, toe_deg=0.0):
    s = 1.0 if side == "L" else -1.0
    q_thigh = qmul(qaxis((0, 0, 1), s * abduct_deg), qaxis((1, 0, 0), -swing_deg))
    q_knee = qaxis((1, 0, 0), knee_deg)
    q_foot = qaxis((1, 0, 0), swing_deg - knee_deg + foot_deg)
    q_toe = qaxis((1, 0, 0), toe_deg)
    return q_thigh, q_knee, q_foot, q_toe

def make_pose(params):
    """params: dict of high level angles -> dict bone->quat + hips offset."""
    pose = {name: (0.0, 0.0, 0.0, 1.0) for name, _ in BONES}
    hip_off = params.get("hip_off", (0.0, 0.0, 0.0))
    lean = params.get("lean", 0.0)
    yaw = params.get("hip_yaw", 0.0)
    chest_yaw = params.get("chest_yaw", 0.0)
    roll = params.get("roll", 0.0)

    pose["Hips"] = qmul(qaxis((0, 1, 0), yaw), qaxis((0, 0, 1), roll))
    pose["Spine"] = qmul(qaxis((1, 0, 0), lean * 0.55), qaxis((0, 1, 0), chest_yaw * 0.45))
    pose["Chest"] = qmul(qaxis((1, 0, 0), lean * 0.45), qaxis((0, 1, 0), chest_yaw))
    pose["Neck"] = qaxis((1, 0, 0), -lean * 0.35 + params.get("head_pitch", 0.0))
    pose["Head"] = qmul(qaxis((1, 0, 0), -lean * 0.30),
                        qaxis((0, 1, 0), params.get("head_yaw", 0.0)))

    for side in ("L", "R"):
        ls = 1.0 if side == "L" else -1.0
        lower = params.get("arm_lower", 76.0) + params.get("arm_lower_" + side, 0.0)
        fwd = params.get("arm_fwd", 0.0) + params.get("arm_fwd_" + side, 0.0)
        elbow = params.get("elbow", 14.0) + params.get("elbow_" + side, 0.0)
        wrist = params.get("wrist", 0.0)
        qu, qe, qw = arm_pose(side, lower, fwd, elbow, wrist)
        pose["Shoulder." + side] = qaxis((0, 0, 1), -ls * params.get("shoulder_lower", 0.0))
        pose["UpperArm." + side] = qu
        pose["LowerArm." + side] = qe
        pose["Hand." + side] = qw

        swing = params.get("thigh_swing_" + side, params.get("thigh_swing", 0.0))
        knee = params.get("knee_" + side, params.get("knee", 4.0))
        abduct = params.get("abduct_" + side, params.get("abduct", 0.0))
        foot = params.get("foot_" + side, params.get("foot", 0.0))
        toe = params.get("toe_" + side, params.get("toe", 0.0))
        qt, qk, qf, qtoe = leg_pose(side, swing, knee, abduct, foot, toe)
        pose["Thigh." + side] = qt
        pose["Shin." + side] = qk
        pose["Foot." + side] = qf
        pose["Toe." + side] = qtoe

    return pose, hip_off

def clip_idle(t):
    p = TAU * t
    params = {
        "arm_lower": 72.0, "elbow": 18.0,
        "arm_fwd_L": 3.0 + 1.5 * math.sin(p), "arm_fwd_R": 3.0 - 1.5 * math.sin(p),
        "knee_L": 5.0, "knee_R": 5.0,
        "lean": 1.5 + 0.8 * math.sin(2 * p),
        "chest_yaw": 1.2 * math.sin(p),
        "head_yaw": 2.5 * math.sin(p * 0.5),
        "hip_off": (0.004 * math.sin(p), -0.002 + 0.004 * math.sin(2 * p), 0.0),
    }
    return make_pose(params)

def clip_walk(t):
    p = TAU * t
    swing = 25.0
    knee_l = 6.0 + 30.0 * max(0.0, -math.sin(p))
    knee_r = 6.0 + 30.0 * max(0.0, math.sin(p))
    params = {
        "arm_lower": 73.0, "elbow": 18.0,
        "arm_fwd_L": -20.0 * math.cos(p), "arm_fwd_R": 20.0 * math.cos(p),
        "thigh_swing_L": swing * math.cos(p), "thigh_swing_R": -swing * math.cos(p),
        "knee_L": knee_l, "knee_R": knee_r,
        "foot_L": 5.0 * math.cos(p), "foot_R": -5.0 * math.cos(p),
        "lean": 2.5, "hip_yaw": 3.0 * math.sin(p), "chest_yaw": -4.0 * math.sin(p),
        "roll": -1.2 * math.sin(p),
        "head_yaw": 1.5 * math.sin(p),
        "hip_off": (0.020 * math.cos(p), -0.012 + 0.012 * math.cos(2 * p), 0.0),
    }
    return make_pose(params)

def clip_run(t):
    p = TAU * t
    swing = 42.0
    knee_l = 14.0 + 52.0 * max(0.0, -math.sin(p))
    knee_r = 14.0 + 52.0 * max(0.0, math.sin(p))
    params = {
        "arm_lower": 78.0, "elbow": 62.0,
        "arm_fwd_L": -42.0 * math.cos(p), "arm_fwd_R": 42.0 * math.cos(p),
        "thigh_swing_L": swing * math.cos(p), "thigh_swing_R": -swing * math.cos(p),
        "knee_L": knee_l, "knee_R": knee_r,
        "foot_L": 10.0 * math.cos(p), "foot_R": -10.0 * math.cos(p),
        "lean": 9.0, "hip_yaw": 5.0 * math.sin(p), "chest_yaw": -7.0 * math.sin(p),
        "roll": -2.0 * math.sin(p),
        "hip_off": (0.028 * math.cos(p), -0.010 + 0.040 * math.cos(2 * p), 0.0),
    }
    return make_pose(params)

def clip_walk_back(t):
    pose, off = clip_walk(-t)
    params = {
        "arm_lower": 73.0, "elbow": 18.0,
        "arm_fwd_L": 14.0 * math.cos(TAU * t), "arm_fwd_R": -14.0 * math.cos(TAU * t),
        "thigh_swing_L": -18.0 * math.cos(TAU * t), "thigh_swing_R": 18.0 * math.cos(TAU * t),
        "knee_L": 10.0, "knee_R": 10.0,
        "lean": -3.0, "hip_yaw": -3.0 * math.sin(TAU * t), "chest_yaw": 3.0 * math.sin(TAU * t),
        "hip_off": (0.018 * math.cos(TAU * t), -0.012 + 0.010 * math.cos(2 * TAU * t), 0.0),
    }
    return make_pose(params)

def clip_strafe(t, direction):
    p = TAU * t
    s = 1.0 if direction > 0 else -1.0
    params = {
        "arm_lower": 70.0, "elbow": 14.0,
        "arm_fwd_L": 3.0 * math.sin(p), "arm_fwd_R": -3.0 * math.sin(p),
        "abduct_L": s * (10.0 + 5.0 * math.sin(p)),
        "abduct_R": -s * (10.0 + 5.0 * math.sin(p)),
        "knee_L": 8.0 + 14.0 * max(0.0, math.sin(p)),
        "knee_R": 8.0 + 14.0 * max(0.0, -math.sin(p)),
        "lean": 2.0, "roll": -s * 3.0,
        "hip_off": (s * 0.024 * math.sin(p), -0.008 + 0.012 * math.cos(2 * p), 0.0),
    }
    return make_pose(params)

def clip_fall(t):
    p = TAU * t
    params = {
        "arm_lower": 38.0, "elbow": 22.0,
        "arm_fwd_L": 12.0 + 6.0 * math.sin(p), "arm_fwd_R": 12.0 - 6.0 * math.sin(p),
        "thigh_swing_L": 12.0 + 6.0 * math.sin(p), "thigh_swing_R": -10.0 - 6.0 * math.sin(p),
        "knee_L": 28.0 + 8.0 * math.sin(p), "knee_R": 22.0 - 8.0 * math.sin(p),
        "lean": -6.0, "head_pitch": -4.0,
        "hip_off": (0.0, 0.0, 0.0),
    }
    return make_pose(params)

def clip_aim(t):
    p = TAU * t
    params = {
        "arm_lower": 84.0, "elbow": 20.0,
        "arm_fwd_L": 76.0 + 1.5 * math.sin(p), "arm_fwd_R": 76.0 - 1.5 * math.sin(p),
        "shoulder_lower": 4.0,
        "lean": 3.0, "chest_yaw": -2.0, "head_pitch": 1.5 + 0.8 * math.sin(p),
        "thigh_swing_L": 0.0, "thigh_swing_R": 0.0,
        "knee_L": 5.0, "knee_R": 5.0,
        "hip_off": (0.0, 0.0, 0.0),
    }
    return make_pose(params)

JUMP_KEYS = [
    (0.00, dict(arm_lower=58.0, arm_fwd=-18.0, elbow=14.0, thigh_swing=-16.0, knee=42.0, lean=14.0, hip_off=(0.0, -0.055, 0.0))),
    (0.16, dict(arm_lower=46.0, arm_fwd=-34.0, elbow=20.0, thigh_swing=-26.0, knee=62.0, lean=18.0, hip_off=(0.0, -0.120, 0.0))),
    (0.36, dict(arm_lower=18.0, arm_fwd=8.0, elbow=8.0, thigh_swing=6.0, knee=6.0, lean=-2.0, hip_off=(0.0, 0.075, 0.0))),
    (0.56, dict(arm_lower=34.0, arm_fwd=22.0, elbow=44.0, thigh_swing=-24.0, knee=68.0, lean=6.0, hip_off=(0.0, 0.020, 0.0))),
    (0.78, dict(arm_lower=52.0, arm_fwd=6.0, elbow=26.0, thigh_swing=-10.0, knee=28.0, lean=4.0, hip_off=(0.0, -0.020, 0.0))),
    (1.00, dict(arm_lower=58.0, arm_fwd=-18.0, elbow=14.0, thigh_swing=-4.0, knee=12.0, lean=6.0, hip_off=(0.0, -0.015, 0.0))),
]

def clip_jump(t):
    t = max(0.0, min(1.0, t))
    keys = JUMP_KEYS
    for i in range(len(keys) - 1):
        t0, a = keys[i]
        t1, b = keys[i + 1]
        if t <= t1:
            f = smoothstep((t - t0) / max(1e-6, t1 - t0))
            params = {}
            for k in a:
                va, vb = a[k], b.get(k, a[k])
                if isinstance(va, tuple):
                    params[k] = tuple(va[j] + (vb[j] - va[j]) * f for j in range(3))
                else:
                    params[k] = va + (vb - va) * f
            return make_pose(params)
    return make_pose(dict(keys[-1][1]))

CLIPS = [
    ("Idle", 2.4, True, clip_idle),
    ("Walk", 1.0, True, clip_walk),
    ("Run", 0.72, True, clip_run),
    ("WalkBack", 1.0, True, clip_walk_back),
    ("StrafeLeft", 1.0, True, lambda t: clip_strafe(t, 1.0)),
    ("StrafeRight", 1.0, True, lambda t: clip_strafe(t, -1.0)),
    ("Jump", 0.9, False, clip_jump),
    ("Fall", 1.0, True, clip_fall),
    ("Aim", 1.2, True, clip_aim),
]

def sample_clip(fn, duration, loop, frame_count):
    frames = []
    for i in range(frame_count + 1):
        t = (i / frame_count) if frame_count else 0.0
        if loop and i == frame_count:
            t = 0.0  # exact loop closure
        pose, hip_off = fn(t)
        frames.append((i / FPS if i < frame_count else duration, pose, hip_off))
    return frames

# ---------------------------------------------------------------------------
# glTF writing
# ---------------------------------------------------------------------------

buf = bytearray()

def align4():
    while len(buf) % 4:
        buf.append(0)

accessors = []
bufferViews = []

def add_view(data, target=None):
    align4()
    off = len(buf)
    buf.extend(data)
    view = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
    if target:
        view["target"] = target
    bufferViews.append(view)
    return len(bufferViews) - 1

def add_accessor(view, comp_type, count, typ, mins=None, maxs=None):
    acc = {"bufferView": view, "componentType": comp_type, "count": count, "type": typ}
    if mins is not None:
        acc["min"] = mins
        acc["max"] = maxs
    accessors.append(acc)
    return len(accessors) - 1

def pack_floats(values):
    return struct.pack("<%df" % len(values), *values)

# --- mesh primitives ---
primitives = []
for mi in sorted(B.pos.keys()):
    if not B.pos[mi]:
        continue
    pos = B.pos[mi]
    nrm = B.nrm[mi]
    joints = B.joints[mi]
    weights = B.weights[mi]
    idx = B.idx[mi]

    flat_pos = [c for p in pos for c in p]
    flat_nrm = [c for n in nrm for c in n]
    flat_j = [c for j in joints for c in j]
    flat_w = [c for w in weights for c in w]

    mins = [min(p[i] for p in pos) for i in range(3)]
    maxs = [max(p[i] for p in pos) for i in range(3)]

    v_pos = add_view(pack_floats(flat_pos), 34962)
    v_nrm = add_view(pack_floats(flat_nrm), 34962)
    v_j = add_view(struct.pack("<%dH" % len(flat_j), *flat_j), 34962)
    v_w = add_view(pack_floats(flat_w), 34962)
    v_idx = add_view(struct.pack("<%dI" % len(idx), *idx), 34963)

    a_pos = add_accessor(v_pos, 5126, len(pos), "VEC3", mins, maxs)
    a_nrm = add_accessor(v_nrm, 5126, len(pos), "VEC3")
    a_j = add_accessor(v_j, 5123, len(pos), "VEC4")
    a_w = add_accessor(v_w, 5126, len(pos), "VEC4")
    a_idx = add_accessor(v_idx, 5125, len(idx), "SCALAR")

    primitives.append({
        "attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "JOINTS_0": a_j, "WEIGHTS_0": a_w},
        "indices": a_idx,
        "material": mi,
        "mode": 4,
    })

mesh_index = 0
mesh_node_index = len(BONES)

# --- skin: inverse bind matrices ---
ibm_bytes = bytearray()
for name, _ in BONES:
    x, y, z = BIND[name]
    # column-major identity rotation, translate(-pos)
    values = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1]
    ibm_bytes.extend(pack_floats(values))
ibm_view = add_view(bytes(ibm_bytes))
ibm_acc = add_accessor(ibm_view, 5126, len(BONES), "MAT4")

# --- nodes ---
nodes = []
for name, parent in BONES:
    px, py, pz = BIND[name]
    if parent:
        qx, qy, qz = BIND[parent]
        t = [px - qx, py - qy, pz - qz]
    else:
        t = [px, py, pz]
    nodes.append({"name": name, "translation": t})
for name, parent in BONES:
    if parent:
        nodes[BONE_INDEX[parent]].setdefault("children", []).append(BONE_INDEX[name])
nodes.append({"name": "MannequinMesh", "mesh": mesh_index, "skin": 0})

# --- animations ---
animations = []
for clip_name, duration, loop, fn in CLIPS:
    frame_count = max(1, int(round(duration * FPS)))
    frames = sample_clip(fn, duration, loop, frame_count)

    # rotation channels for all bones, translation for Hips (+ Root for completeness)
    times = [f[0] for f in frames]
    t_view = add_view(pack_floats(times))
    t_acc = add_accessor(t_view, 5126, len(times), "SCALAR", [min(times)], [max(times)])

    channels = []
    samplers = []

    def build_channel(bone_name, path, values, typ):
        v = add_view(pack_floats([c for val in values for c in val]))
        a = add_accessor(v, 5126, len(values), typ)
        sampler = {"input": t_acc, "output": a, "interpolation": "LINEAR"}
        channel = {"sampler": len(samplers), "target": {"node": BONE_INDEX[bone_name], "path": path}}
        return channel, sampler

    def add_channel(bone_name, path, values, typ):
        channel, sampler = build_channel(bone_name, path, values, typ)
        channels.append(channel)
        samplers.append(sampler)

    for name, _ in BONES:
        if name == "Root":
            continue
        quats = []
        prev = None
        for _, pose, _ in frames:
            q = qnorm(pose[name])
            if prev is not None and (q[0] * prev[0] + q[1] * prev[1] + q[2] * prev[2] + q[3] * prev[3]) < 0.0:
                q = (-q[0], -q[1], -q[2], -q[3])
            prev = q
            quats.append(q)
        add_channel(name, "rotation", quats, "VEC4")

    # hips translation (bind local + offset)
    hip_bind = BIND["Hips"]
    hips_vals = []
    for _, _, off in frames:
        hips_vals.append((hip_bind[0] + off[0], hip_bind[1] + off[1], hip_bind[2] + off[2]))
    add_channel("Hips", "translation", hips_vals, "VEC3")

    animations.append({"name": clip_name, "channels": channels, "samplers": samplers})

# --- assemble ---
buffer_uri = "data:application/octet-stream;base64," + base64.b64encode(bytes(buf)).decode("ascii")

materials = []
for name, base, metallic, rough, emissive in MATERIALS:
    m = {
        "name": name,
        "doubleSided": False,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(base),
            "metallicFactor": metallic,
            "roughnessFactor": rough,
        },
    }
    if any(c > 0.0 for c in emissive):
        m["emissiveFactor"] = list(emissive)
    materials.append(m)

doc = {
    "asset": {"version": "2.0", "generator": "WorldEngine Mannequin Generator 1.0"},
    "scene": 0,
    "scenes": [{"name": "MannequinScene", "nodes": [0, mesh_node_index]}],
    "nodes": nodes,
    "meshes": [{"name": "Mannequin", "primitives": primitives}],
    "materials": materials,
    "skins": [{
        "name": "MannequinRig",
        "joints": list(range(len(BONES))),
        "inverseBindMatrices": ibm_acc,
        "skeleton": 0,
    }],
    "animations": animations,
    "accessors": accessors,
    "bufferViews": bufferViews,
    "buffers": [{"byteLength": len(buf), "uri": buffer_uri}],
}

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    json.dump(doc, f, indent=1)

verts = sum(len(v) for v in B.pos.values())
tris = sum(len(v) for v in B.idx.values()) // 3
print("Wrote", OUT)
print("  bones=%d meshes=%d verts=%d tris=%d materials=%d clips=%d" %
      (len(BONES), len(primitives), verts, tris, len(materials), len(animations)))
for name, duration, loop, _ in CLIPS:
    print("  clip %-12s %.2fs %s" % (name, duration, "loop" if loop else "once"))



