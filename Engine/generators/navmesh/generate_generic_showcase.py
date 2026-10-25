#!/usr/bin/env python3
"""
WorldEngine — Generic Navigation Showcase NavMesh Generator
Generates:
  1. templates/project-example/assets/scenes/GenericNavigationShowcase.wnavmesh (.wnavmesh v2 binary)
  2. projects/assets/scenes/GenericNavigationShowcase.wnavmesh
"""

import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))

OUT_NAVMESHES = [
    os.path.join(REPO, "templates", "project-example", "assets", "scenes", "GenericNavigationShowcase.wnavmesh"),
    os.path.join(REPO, "projects", "assets", "scenes", "GenericNavigationShowcase.wnavmesh")
]

def generate_generic_navmesh():
    vertices = []
    polygons = []
    v_map = {}

    def get_or_add_vertex(pt):
        key = (round(pt[0], 3), round(pt[1], 3), round(pt[2], 3))
        if key in v_map:
            return v_map[key]
        idx = len(vertices)
        vertices.append(pt)
        v_map[key] = idx
        return idx

    def add_quad(p0, p1, p2, p3):
        i0 = get_or_add_vertex(p0)
        i1 = get_or_add_vertex(p1)
        i2 = get_or_add_vertex(p2)
        i3 = get_or_add_vertex(p3)
        polygons.append({
            "indices": [i0, i1, i2, i3],
            "neighbors": [-1, -1, -1, -1]
        })

    # Ground floor: 40m x 40m centered at origin, subdivided into 2m x 2m quads
    for zi in range(-10, 10):
        for xi in range(-10, 10):
            x0, x1 = float(xi * 2), float((xi + 1) * 2)
            z0, z1 = float(zi * 2), float((zi + 1) * 2)
            add_quad((x0, 0.0, z0), (x1, 0.0, z0), (x1, 0.0, z1), (x0, 0.0, z1))

    # Second floor balcony: 16m x 10m at Y=3.0, X in [-8, 8], Z in [16, 26]
    for zi in range(8, 13):
        for xi in range(-4, 4):
            x0, x1 = float(xi * 2), float((xi + 1) * 2)
            z0, z1 = float(zi * 2), float((zi + 1) * 2)
            add_quad((x0, 3.0, z0), (x1, 3.0, z0), (x1, 3.0, z1), (x0, 3.0, z1))

    # Staircase / Ramp connecting Ground (Z=8) to Balcony (Z=16), X in [-2, 2], rising from Y=0 to Y=3
    for s in range(4):
        z_start = 8.0 + float(s) * 2.0
        z_end = z_start + 2.0
        y_start = float(s) * 0.75
        y_end = float(s + 1) * 0.75
        for xi in range(-1, 1):
            x0, x1 = float(xi * 2), float((xi + 1) * 2)
            add_quad((x0, y_start, z_start), (x1, y_start, z_start), (x1, y_end, z_end), (x0, y_end, z_end))

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

    # OffMeshLinks
    off_mesh_links = [
        # Balcony Jump Drop: from 2nd floor balcony front (0, 3.0, 16.0) down to ground (0, 0.0, 14.0)
        {
            "start": (0.0, 3.0, 16.0),
            "end": (0.0, 0.0, 14.0),
            "cost": 1.0,
            "link_type": 1,
            "bidi": True
        }
    ]

    # AreaModifiers
    area_modifiers = [
        # Marble Corridor (Road, AreaType=1, Cost=0.5): along central axis X in [-3, 3], Z in [-16, 8]
        {
            "center": (0.0, 0.0, -4.0),
            "extents": (3.0, 2.0, 12.0),
            "cost_multiplier": 0.5,
            "area_type": 1 # Road (Bright Green)
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

            # NAV2 extended metadata: magic = 0x4E415632
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
                f.write(struct.pack("<ffffffffBxxxxxxx", cx, cy, cz, ex, ey, ez, mod["cost_multiplier"], mod.get("clearance_height", 0.0), mod["area_type"]))

        print(f"[NavMesh] Generated '{out_path}' ({os.path.getsize(out_path)} bytes)")
    print(f"[NavMesh] Vertices: {len(vertices)}, Polygons: {len(polygons)}, Links: {len(off_mesh_links)}, Modifiers: {len(area_modifiers)}")

if __name__ == "__main__":
    generate_generic_navmesh()
    print("[Done] Generic Navigation Showcase NavMesh generated successfully.")
