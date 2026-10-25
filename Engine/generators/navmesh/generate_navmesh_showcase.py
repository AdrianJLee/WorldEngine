#!/usr/bin/env python3
"""
WorldEngine — 3D NavMesh Showcase Asset Generator
Generates:
  1. templates/project-example/assets/terrains/navmesh_showcase.wterrain (.wterrain v1 binary)
  2. projects/assets/terrains/navmesh_showcase.wterrain
  3. templates/project-example/assets/scenes/NavMeshShowcase.wnavmesh (.wnavmesh v2 binary with NAV2 metadata)
  4. projects/assets/scenes/NavMeshShowcase.wnavmesh
"""

import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))

OUT_TERRAINS = [
    os.path.join(REPO, "templates", "project-example", "assets", "terrains", "navmesh_showcase.wterrain"),
    os.path.join(REPO, "projects", "assets", "terrains", "navmesh_showcase.wterrain")
]

OUT_NAVMESHES = [
    os.path.join(REPO, "templates", "project-example", "assets", "scenes", "NavMeshShowcase.wnavmesh"),
    os.path.join(REPO, "projects", "assets", "scenes", "NavMeshShowcase.wnavmesh")
]

W = H = 65
CELL = 1.0
HEIGHT_SCALE = 1.0
STEP_SIZE = 2.0
MAX_SLOPE_DEG = 45.0

def smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)

def clamp(v, low, high):
    return max(low, min(high, v))

def terrain_height(x, z):
    # Boundary mountain rim
    rim_w = smoothstep(8.0, 2.0, x) * 14.0
    rim_e = smoothstep(56.0, 62.0, x) * 14.0
    rim_s = smoothstep(8.0, 2.0, z) * 14.0
    rim_n = smoothstep(56.0, 62.0, z) * 14.0
    rim = max(rim_w, rim_e, rim_s, rim_n)

    # West Plateau: top at Y = 5.5
    # Plateau top: X in [12, 22], Z in [26, 38]
    # East cliff of plateau: drops sharply between X = 23 and X = 28.5 (steep >50 deg)
    # South gentle ramp: Z in [13, 25], X in [12, 22] (slope ~ 24 deg)
    # North gentle ramp: Z in [39, 51], X in [12, 22] (slope ~ 24 deg)
    if 8.0 <= x <= 29.0 and 12.0 <= z <= 52.0:
        x_factor = 1.0
        if x > 23.0:
            x_factor = 1.0 - smoothstep(23.0, 28.5, x)
        elif x < 12.0:
            x_factor = smoothstep(8.0, 12.0, x)

        z_factor = 0.0
        if 13.0 <= z < 25.0:
            z_factor = (z - 13.0) / 12.0
        elif 25.0 <= z <= 39.0:
            z_factor = 1.0
        elif 39.0 < z <= 51.0:
            z_factor = 1.0 - (z - 39.0) / 12.0

        west_plateau = 5.5 * x_factor * z_factor
    else:
        west_plateau = 0.0

    # East Ridge: steep cliff facing canyon from X=34.5 to X=38.5 (rise 6.0m over 4m -> slope 56 deg)
    # Rocky Outpost plateau / trail (X in [44, 52])
    if 34.5 <= x <= 56.0 and 12.0 <= z <= 52.0:
        if x <= 42.0:
            x_wall = smoothstep(34.5, 38.5, x) * (1.0 - smoothstep(41.0, 43.0, x))
            z_wall = smoothstep(12.0, 20.0, z) * (1.0 - smoothstep(44.0, 52.0, z))
            wall = 6.0 * x_wall * z_wall
        else:
            wall = 0.0

        if 42.0 <= x <= 54.0:
            x_terrace = smoothstep(42.0, 45.0, x) * (1.0 - smoothstep(51.0, 55.0, x))
            z_terrace = 0.0
            if 14.0 <= z < 26.0:
                z_terrace = (z - 14.0) / 12.0
            elif 26.0 <= z <= 38.0:
                z_terrace = 1.0
            elif 38.0 < z <= 50.0:
                z_terrace = 1.0 - (z - 38.0) / 12.0
            terrace = 2.5 * x_terrace * z_terrace
        else:
            terrace = 0.0

        east_hill = max(wall, terrace)
    else:
        east_hill = 0.0

    ripple = 0.02 * math.sin(x * 0.3) * math.cos(z * 0.25)
    h = max(west_plateau, east_hill) + rim + ripple

    # Canyon corridor floor: centered at X=32, width: X in [29.5, 34.5]
    if 29.5 <= x <= 34.5 and 8.0 <= z <= 56.0:
        h = max(0.0, ripple * 0.1)

    return h

def sample_normal(x, z):
    hl = terrain_height(x - 1.0, z)
    hr = terrain_height(x + 1.0, z)
    hd = terrain_height(x, z - 1.0)
    hu = terrain_height(x, z + 1.0)
    nx = hl - hr
    ny = 2.0
    nz = hd - hu
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    return (nx / length, ny / length, nz / length)

def sample_slope(x, z):
    _, ny, _ = sample_normal(x, z)
    ny = clamp(ny, 0.0, 1.0)
    return math.acos(ny) * 180.0 / math.pi

def generate_terrain():
    heights = []
    splat = []

    for z in range(H):
        for x in range(W):
            h = terrain_height(float(x), float(z))
            heights.append(h)

            slope = sample_slope(float(x), float(z))
            slope_norm = min(1.0, slope / 60.0)
            high_norm = smoothstep(1.5, 5.0, h)

            # Corridor dirt path
            in_corridor = 1.0 if (29.0 <= x <= 35.0 and 8.0 <= z <= 56.0) else 0.0

            # Weights: 0=Grass, 1=Dirt, 2=Rock, 3=Sand
            dirt = min(1.0, in_corridor * 0.85 + (0.2 if 14.0 <= z <= 50.0 and 42.0 <= x <= 52.0 else 0.0))
            rock = min(1.0, slope_norm * 0.95 + high_norm * 0.4)
            sand = 0.15 if (h < 0.2 and not in_corridor) else 0.0
            grass = max(0.0, 1.0 - dirt - rock - sand)

            total = grass + dirt + rock + sand + 1e-6
            grass, dirt, rock, sand = grass / total, dirt / total, rock / total, sand / total

            packed = (int(round(grass * 255)) |
                      (int(round(dirt * 255)) << 8) |
                      (int(round(rock * 255)) << 16) |
                      (int(round(sand * 255)) << 24))
            splat.append(packed)

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

    for out_t in OUT_TERRAINS:
        os.makedirs(os.path.dirname(out_t), exist_ok=True)
        with open(out_t, "wb") as f:
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
        print(f"[Terrain] Generated '{out_t}' ({os.path.getsize(out_t)} bytes)")

def generate_navmesh():
    world_w = float(W - 1) * CELL
    world_h = float(H - 1) * CELL
    num_steps_x = max(1, int(world_w / STEP_SIZE))
    num_steps_z = max(1, int(world_h / STEP_SIZE))

    vertices = []
    grid = [[0 for _ in range(num_steps_x + 1)] for _ in range(num_steps_z + 1)]

    for z in range(num_steps_z + 1):
        for x in range(num_steps_x + 1):
            wx = min(float(x) * STEP_SIZE, world_w)
            wz = min(float(z) * STEP_SIZE, world_h)
            wy = terrain_height(wx, wz)
            vertices.append((wx, wy, wz))
            grid[z][x] = len(vertices) - 1

    polygons = []
    for z in range(num_steps_z):
        for x in range(num_steps_x):
            wx = float(x) * STEP_SIZE + STEP_SIZE * 0.5
            wz = float(z) * STEP_SIZE + STEP_SIZE * 0.5
            slope = sample_slope(wx, wz)
            if slope > MAX_SLOPE_DEG:
                continue

            v0 = grid[z][x]
            v1 = grid[z][x + 1]
            v2 = grid[z + 1][x + 1]
            v3 = grid[z + 1][x]
            polygons.append({
                "indices": [v0, v1, v2, v3],
                "neighbors": [-1, -1, -1, -1]
            })

    # Build adjacency
    edge_map = {}
    for p_idx, poly in enumerate(polygons):
        v_count = len(poly["indices"])
        for e in range(v_count):
            i0 = poly["indices"][e]
            i1 = poly["indices"][(e + 1) % v_count]
            key = (min(i0, i1), max(i0, i1))
            if key in edge_map:
                other_p, other_e = edge_map[key]
                poly["neighbors"][e] = other_p
                polygons[other_p]["neighbors"][other_e] = p_idx
            else:
                edge_map[key] = (p_idx, e)

    # Off-Mesh Links (洋红色抛物线跳跃点)
    off_mesh_links = [
        # 1. 西侧高台跳跃点 (Plateau Leap): 从高台顶部 (24, 5.2, 30) 纵身跃下至峡谷 (30, 0.1, 30)
        {
            "start": (24.0, 5.2, 30.0),
            "end": (30.0, 0.1, 30.0),
            "cost": 1.2,
            "link_type": 1,
            "bidi": True
        },
        # 2. 东侧岩石前哨跳跃点 (Rocky Outpost Leap): 峡谷步道 (34, 0.1, 34) 攀爬跳跃上岩石平台 (42, 2.4, 34)
        {
            "start": (34.0, 0.1, 34.0),
            "end": (42.0, 2.4, 34.0),
            "cost": 1.5,
            "link_type": 2,
            "bidi": True
        }
    ]

    # Area Modifiers (区域代价修饰区: 道路 0.5x, 泥泞 3.0x, 熔岩 10.0x)
    area_modifiers = [
        # 1. 石板古道 (Cobblestone Highway): 沿中央峡谷主干道，通行代价 0.5 (加速道路，浅绿/亮绿色)
        {
            "center": (32.0, 0.0, 20.0),
            "extents": (3.5, 3.0, 8.0),
            "cost_multiplier": 0.5,
            "area_type": 1 # Road (Bright Green)
        },
        # 2. 毒沼烂泥 (Deep Mud Swamp): 西南低洼湿地盆地，通行代价 3.0 (高阻力泥泞，紫色)
        {
            "center": (20.0, 0.0, 44.0),
            "extents": (6.0, 3.0, 6.0),
            "cost_multiplier": 3.0,
            "area_type": 2 # Mud (Purple)
        },
        # 3. 熔岩地裂 (Lava Fissure / Hazard): 东南岩石裂隙险境，通行代价 10.0 (危险避让区，橙红色)
        {
            "center": (46.0, 2.0, 20.0),
            "extents": (5.0, 3.0, 4.0),
            "cost_multiplier": 10.0,
            "area_type": 3 # Hazard (Orange-Red)
        }
    ]

    # Assign polygon AreaType and Cost
    for poly in polygons:
        cx = sum(vertices[vi][0] for vi in poly["indices"]) / len(poly["indices"])
        cy = sum(vertices[vi][1] for vi in poly["indices"]) / len(poly["indices"])
        cz = sum(vertices[vi][2] for vi in poly["indices"]) / len(poly["indices"])
        poly["center"] = (cx, cy, cz)
        poly["area_type"] = 0
        poly["cost"] = 1.0

        for mod in area_modifiers:
            mc = mod["center"]
            me = mod["extents"]
            if (abs(cx - mc[0]) <= me[0] and
                abs(cy - mc[1]) <= me[1] and
                abs(cz - mc[2]) <= me[2]):
                poly["area_type"] = mod["area_type"]
                poly["cost"] = mod["cost_multiplier"]
                break

    # Write .wnavmesh binary with v2 NAV2 extended metadata
    for out_path in OUT_NAVMESHES:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "wb") as f:
            v_count = len(vertices)
            f.write(struct.pack("<I", v_count))
            for vx, vy, vz in vertices:
                f.write(struct.pack("<fff", vx, vy, vz))

            p_count = len(polygons)
            f.write(struct.pack("<I", p_count))
            for poly in polygons:
                indices = poly["indices"]
                f.write(struct.pack("<I", len(indices)))
                f.write(struct.pack("<%dI" % len(indices), *indices))

                neighbors = poly["neighbors"]
                f.write(struct.pack("<I", len(neighbors)))
                f.write(struct.pack("<%di" % len(neighbors), *neighbors))

            # NAV2 extended metadata: magic = 0x4E415632 (b'2VAN' in LE)
            f.write(struct.pack("<I", 0x4E415632))

            for poly in polygons:
                f.write(struct.pack("B", poly["area_type"]))
                f.write(struct.pack("<f", poly["cost"]))

            f.write(struct.pack("<I", len(off_mesh_links)))
            for link in off_mesh_links:
                sx, sy, sz = link["start"]
                ex, ey, ez = link["end"]
                f.write(struct.pack("<fffffffBBxx", sx, sy, sz, ex, ey, ez, link["cost"], link["link_type"], 1 if link["bidi"] else 0))

            f.write(struct.pack("<I", len(area_modifiers)))
            for mod in area_modifiers:
                cx, cy, cz = mod["center"]
                ex, ey, ez = mod["extents"]
                f.write(struct.pack("<fffffffBxxx", cx, cy, cz, ex, ey, ez, mod["cost_multiplier"], mod["area_type"]))

        print(f"[NavMesh] Generated '{out_path}' ({os.path.getsize(out_path)} bytes)")
    print(f"[NavMesh] Vertices: {len(vertices)}, Polygons: {len(polygons)}, Links: {len(off_mesh_links)}, Modifiers: {len(area_modifiers)}")

if __name__ == "__main__":
    generate_terrain()
    generate_navmesh()
    print("[Done] All showcase assets generated successfully.")
