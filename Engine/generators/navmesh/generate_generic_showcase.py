#!/usr/bin/env python3
"""
WorldEngine — Generic 3D Navigation & Multi-Level Architectural Showcase Asset Generator
Generates:
  1. templates/project-example/assets/scenes/GenericNavigationShowcase.wlevel
  2. templates/project-example/assets/scenes/GenericNavigationShowcase.wnavmesh (.wnavmesh binary)
  3. templates/project-example/assets/levels/GenericNavigationShowcase/entities/*.entity
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SCENES_DIR = os.path.join(REPO, "templates", "project-example", "assets", "scenes")
LEVELS_DIR = os.path.join(REPO, "templates", "project-example", "assets", "levels", "GenericNavigationShowcase")
ENTITIES_DIR = os.path.join(LEVELS_DIR, "entities")

OUT_WLEVEL = os.path.join(SCENES_DIR, "GenericNavigationShowcase.wlevel")
OUT_NAVMESH = os.path.join(SCENES_DIR, "GenericNavigationShowcase.wnavmesh")

def build_generic_navmesh():
    os.makedirs(SCENES_DIR, exist_ok=True)
    vertices = []
    vertex_map = {}

    def get_or_add_vertex(x, y, z):
        key = (round(x, 3), round(y, 3), round(z, 3))
        if key in vertex_map:
            return vertex_map[key]
        idx = len(vertices)
        vertices.append((float(key[0]), float(key[1]), float(key[2])))
        vertex_map[key] = idx
        return idx

    polygons = []

    def add_quad(p0, p1, p2, p3):
        i0 = get_or_add_vertex(*p0)
        i1 = get_or_add_vertex(*p1)
        i2 = get_or_add_vertex(*p2)
        i3 = get_or_add_vertex(*p3)
        polygons.append({
            "indices": [i0, i1, i2, i3],
            "neighbors": [-1, -1, -1, -1]
        })

    # =========================================================================
    # 1. Ground Floor (1F) Grid: 40m x 40m (X: [2, 38], Z: [2, 38])
    # Step size: 2m
    # =========================================================================
    step = 2.0
    for zi in range(1, 19): # Z from 2.0 to 38.0
        z0 = float(zi) * step
        z1 = float(zi + 1) * step
        for xi in range(1, 19): # X from 2.0 to 38.0
            x0 = float(xi) * step
            x1 = float(xi + 1) * step

            # Pillar cutouts (SW, SE, NW, NE)
            if 10.0 <= x0 < 12.0 and 10.0 <= z0 < 12.0:
                continue
            if 28.0 <= x0 < 30.0 and 10.0 <= z0 < 12.0:
                continue
            if 10.0 <= x0 < 12.0 and 28.0 <= z0 < 30.0:
                continue
            if 28.0 <= x0 < 30.0 and 28.0 <= z0 < 30.0:
                continue

            # North partition walls (West & East, Doorway at [18..22] open)
            if 30.0 <= z0 < 32.0:
                if 8.0 <= x0 < 18.0 or 22.0 <= x0 < 32.0:
                    continue

            # Staircase footprint (handled by stair treads)
            if 4.0 <= x0 < 8.0 and 8.0 <= z0 < 16.0:
                continue

            # East ramp footprint (handled by ramp slopes)
            if 32.0 <= x0 < 36.0 and 14.0 <= z0 < 26.0:
                continue

            add_quad(
                (x0, 0.0, z0),
                (x1, 0.0, z0),
                (x1, 0.0, z1),
                (x0, 0.0, z1)
            )

    # =========================================================================
    # 2. Grand Staircase (双阶直跑台阶楼梯)
    # Climbing North from Z=8.0 (Y=0.0) to Z=16.0 (Y=3.0)
    # Width: X in [4.0, 8.0] (subdivided at X=6.0 to align with 2m grid)
    # 8 consecutive steps (each 1.0m in Z, rise 0.375m)
    # =========================================================================
    num_steps = 8
    for xi in [4.0, 6.0]:
        x0 = xi
        x1 = xi + 2.0
        for s in range(num_steps):
            s_z0 = 8.0 + float(s) * 1.0
            s_z1 = 8.0 + float(s + 1) * 1.0
            s_y0 = float(s) * (3.0 / num_steps)
            s_y1 = float(s + 1) * (3.0 / num_steps)

            add_quad(
                (x0, s_y0, s_z0),
                (x1, s_y0, s_z0),
                (x1, s_y1, s_z1),
                (x0, s_y1, s_z1)
            )

    # =========================================================================
    # 3. Second Floor Balcony & Skybridge (2F)
    # Elevation: Y = 3.0m
    # Skybridge Corridor: X in [4.0, 36.0], Z in [16.0, 20.0]
    # Balcony Platform: X in [16.0, 24.0], Z in [20.0, 26.0]
    # East Corridor Extension to Ramp: X in [32.0, 36.0], Z in [20.0, 26.0]
    # =========================================================================
    bridge_y = 3.0

    # Skybridge Corridor: X in [4.0, 36.0], Z in [16.0, 20.0] (2m grid)
    for bx in range(2, 18):
        bx0 = float(bx) * 2.0
        bx1 = float(bx + 1) * 2.0
        for bz in [16.0, 18.0]:
            bz0 = bz
            bz1 = bz + 2.0
            add_quad(
                (bx0, bridge_y, bz0),
                (bx1, bridge_y, bz0),
                (bx1, bridge_y, bz1),
                (bx0, bridge_y, bz1)
            )

    # Central Balcony Platform: X in [16.0, 24.0], Z in [20.0, 26.0]
    for bx in range(8, 12):
        bx0 = float(bx) * 2.0
        bx1 = float(bx + 1) * 2.0
        for bz in [20.0, 22.0, 24.0]:
            bz0 = bz
            bz1 = bz + 2.0
            add_quad(
                (bx0, bridge_y, bz0),
                (bx1, bridge_y, bz0),
                (bx1, bridge_y, bz1),
                (bx0, bridge_y, bz1)
            )

    # East corridor extension to ramp top: X in [32.0, 36.0], Z in [20.0, 26.0]
    for bx in [32.0, 34.0]:
        bx0 = bx
        bx1 = bx + 2.0
        for bz in [20.0, 22.0, 24.0]:
            bz0 = bz
            bz1 = bz + 2.0
            add_quad(
                (bx0, bridge_y, bz0),
                (bx1, bridge_y, bz0),
                (bx1, bridge_y, bz1),
                (bx0, bridge_y, bz1)
            )

    # =========================================================================
    # 4. East Descent Ramp (单向斜坡引道)
    # Descends South from Z=26.0 (Y=3.0) to Z=14.0 (Y=0.0)
    # Width: X in [32.0, 36.0] (subdivided at X=34.0)
    # 6 steps (each 2m in Z, slope 0.5m)
    # =========================================================================
    ramp_steps = 6
    for xi in [32.0, 34.0]:
        x0 = xi
        x1 = xi + 2.0
        for r in range(ramp_steps):
            rz_top = 26.0 - float(r) * 2.0
            rz_bot = 26.0 - float(r + 1) * 2.0
            ry_top = 3.0 - float(r) * 0.5
            ry_bot = 3.0 - float(r + 1) * 0.5

            add_quad(
                (x0, ry_bot, rz_bot),
                (x1, ry_bot, rz_bot),
                (x1, ry_top, rz_top),
                (x0, ry_top, rz_top)
            )

    # =========================================================================
    # 5. Off-Mesh Link / Balcony Jump Drop Connection
    # Connects balcony front edge at (X in [18, 22], Y=3.0, Z=16.0)
    # down to landing pad cushion at (X in [18, 22], Y=0.0, Z=14.0)
    # Subdivided at X=20.0 to match the 2m grid
    # =========================================================================
    for xi in [18.0, 20.0]:
        x0 = xi
        x1 = xi + 2.0
        add_quad(
            (x0, 0.0, 14.0),
            (x1, 0.0, 14.0),
            (x1, 3.0, 16.0),
            (x0, 3.0, 16.0)
        )

    # =========================================================================
    # Build Adjacency
    # =========================================================================
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

    # Write .wnavmesh binary
    with open(OUT_NAVMESH, "wb") as f:
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

    print(f"[NavMesh] Generated '{OUT_NAVMESH}' ({os.path.getsize(OUT_NAVMESH)} bytes)")
    print(f"[NavMesh] Vertices: {len(vertices)}, Polygons: {len(polygons)}")

def build_level_and_entities():
    os.makedirs(ENTITIES_DIR, exist_ok=True)

    with open(OUT_WLEVEL, "w", encoding="utf-8") as f:
        f.write("""Id: GenericNavigationShowcase
Name: Generic 3D Navigation & Multi-Level Architectural Showcase
GridCellSize: 64
DataLayers:
  - Mask: 1
    Name: Base_World
    DefaultActive: true
""")
    print(f"[Level] Generated '{OUT_WLEVEL}'")

    def write_entity(entity_id, tag, transform, components=""):
        path = os.path.join(ENTITIES_DIR, f"{entity_id}.entity")
        loc = transform.get("Location", [0, 0, 0])
        rot = transform.get("Rotation", [0, 0, 0])
        scale = transform.get("Scale", [1, 1, 1])

        content = f"""ChunkID: 0
Entity:
  World::UUIDComponent:
    ID:
      m_UUID: {entity_id}
  World::TagComponent:
    Tag: "{tag}"
  World::TransformComponent:
    Location: [{loc[0]}, {loc[1]}, {loc[2]}]
    Rotation: [{rot[0]}, {rot[1]}, {rot[2]}]
    Scale: [{scale[0]}, {scale[1]}, {scale[2]}]
{components}  World::LevelEntityComponent:
    DataLayerMask: 1
"""
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)

    # 1. AI Mannequin Agent
    write_entity(
        970000000000000001,
        "Mannequin Agent",
        {"Location": [20.0, 0.0, 4.0]},
        """  World::SkinnedMeshRendererComponent:
    Mesh: "engine/models/characters/mannequin.wmodel"
    MeshIndex: 0
    Material: ""
    AnimationClip: ""
    Speed: 1.0
    Time: 0.0
    Playing: true
    Loop: true
  World::AnimGraphComponent:
    AnimGraphAsset: "engine/animation/mannequin_locomotion.wanimgraph"
    PlaybackSpeed: 1.0
    RootMotionWeight: 1.0
    FootIkWeight: 1.0
    CurrentStateHash: 0
    EnableRootMotion: 0
    EnableFootIk: 0
    EnableUpperBodyLayer: 0
  World::NavMeshAgentComponent:
    Destination: [20.0, 3.0, 24.0]
    Speed: 4.5
    Acceleration: 8.0
    StoppingDistance: 0.3
    AgentRadius: 0.5
    AgentHeight: 1.8
    HasPath: 0
    ReachedDestination: 0
    AutoRepath: 1
"""
    )

    # 2. Ground Floor Hall (40x40m, Y=0.0m)
    write_entity(
        970000000000000002,
        "Ground Floor Hall",
        {"Location": [20.0, -0.2, 20.0]},
        """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [40.0, 0.4, 40.0]
    BevelSize: 0.0
    GenerateCollision: 1
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.32, 0.35, 0.40, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 3. Grand Staircase (Stairs, 8 steps, Y=0.0m -> 3.0m)
    write_entity(
        970000000000000003,
        "Grand Staircase",
        {"Location": [6.0, 1.5, 12.0]},
        """  World::WhiteboxComponent:
    Shape: 2
    Subdivisions: 1
    Steps: 8
    Dimensions: [4.0, 3.0, 8.0]
    BevelSize: 0.0
    GenerateCollision: 1
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.72, 0.75, 0.80, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 4. Second Floor Balcony & Skybridge (20x8m, Y=3.0m)
    write_entity(
        970000000000000004,
        "Second Floor Balcony",
        {"Location": [20.0, 2.85, 21.0]},
        """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [20.0, 0.3, 10.0]
    BevelSize: 0.0
    GenerateCollision: 1
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.46, 0.50, 0.58, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 5. East Descent Ramp (Wedge, descending from 2F to 1F)
    write_entity(
        970000000000000005,
        "East Descent Ramp",
        {"Location": [34.0, 1.5, 20.0]},
        """  World::WhiteboxComponent:
    Shape: 3
    Subdivisions: 1
    Steps: 8
    Dimensions: [4.0, 3.0, 12.0]
    BevelSize: 0.0
    GenerateCollision: 1
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.58, 0.62, 0.68, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 6. Courtyard Mud Puddle (Cost=3.0)
    write_entity(
        970000000000000006,
        "Courtyard Mud Puddle",
        {"Location": [20.0, 0.03, 20.0]},
        """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [12.0, 0.06, 12.0]
    BevelSize: 0.0
    GenerateCollision: 0
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.42, 0.28, 0.16, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 7. West Paved Road (Cost=0.5)
    write_entity(
        970000000000000007,
        "West Paved Road",
        {"Location": [6.0, 0.02, 20.0]},
        """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [3.5, 0.04, 34.0]
    BevelSize: 0.0
    GenerateCollision: 0
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.82, 0.78, 0.64, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 8. East Paved Road (Cost=0.5)
    write_entity(
        970000000000000008,
        "East Paved Road",
        {"Location": [34.0, 0.02, 20.0]},
        """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [3.5, 0.04, 34.0]
    BevelSize: 0.0
    GenerateCollision: 0
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.82, 0.78, 0.64, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 9. Jump Drop Landing Pad Cushion
    write_entity(
        970000000000000009,
        "Jump Drop Landing Pad",
        {"Location": [20.0, 0.08, 14.0]},
        """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [3.5, 0.16, 3.5]
    BevelSize: 0.0
    GenerateCollision: 0
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.95, 0.45, 0.15, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 10. Landmark 1: 2nd Floor Balcony Beacon
    write_entity(
        970000000000000010,
        "2nd Floor Balcony Beacon",
        {"Location": [20.0, 3.8, 24.0]},
        """  World::MeshRendererComponent:
    Primitive: 1
    MeshIndex: 0
    Color: [0.15, 0.85, 1.0, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 11. Landmark 2: Courtyard Gate Beacon
    write_entity(
        970000000000000011,
        "Courtyard Gate Beacon",
        {"Location": [20.0, 0.8, 36.0]},
        """  World::MeshRendererComponent:
    Primitive: 1
    MeshIndex: 0
    Color: [0.15, 1.0, 0.35, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 12. Landmark 3: Jump Drop Landing Zone Beacon
    write_entity(
        970000000000000012,
        "Jump Drop Landing Beacon",
        {"Location": [20.0, 0.8, 14.0]},
        """  World::MeshRendererComponent:
    Primitive: 1
    MeshIndex: 0
    Color: [1.0, 0.65, 0.1, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 13. Landmark 4: Entrance Lobby Beacon
    write_entity(
        970000000000000013,
        "Entrance Lobby Beacon",
        {"Location": [20.0, 0.8, 4.0]},
        """  World::MeshRendererComponent:
    Primitive: 1
    MeshIndex: 0
    Color: [0.95, 0.25, 0.25, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    # 14. Showcase Camera
    write_entity(
        970000000000000014,
        "Generic Navigation Camera",
        {"Location": [20.0, 16.0, -4.0], "Rotation": [-0.52359877, 0, 0]},
        """  World::CameraComponent:
    Camera:
      m_ProjectionType: 0
      m_OrthographicZoom: 1
      m_OrthographicNearClip: -1
      m_OrthographicFarClip: 1
      m_PerspectiveFOV: 55
      m_PerspectiveNearClip: 0.1
      m_PerspectiveFarClip: 300
    Primary: true
    FixedAspectRatio: false
  World::AudioListenerComponent:
    Active: true
    VolumeScale: 1.0
"""
    )

    # 15. Showcase Sun
    write_entity(
        970000000000000015,
        "Showcase Sun",
        {"Location": [20.0, 35.0, 20.0], "Rotation": [-55, 35, 0]},
        """  World::DirectionalLightComponent:
    Color: [1.0, 0.96, 0.90]
    Intensity: 2.2
    Direction: [0.4, -0.85, 0.35]
    CastShadow: false
"""
    )

    # 16..19: Architectural Columns
    pillars = [
        (970000000000000016, "Pillar SW", [11.0, 2.5, 11.0]),
        (970000000000000017, "Pillar SE", [29.0, 2.5, 11.0]),
        (970000000000000018, "Pillar NW", [11.0, 2.5, 29.0]),
        (970000000000000019, "Pillar NE", [29.0, 2.5, 29.0]),
    ]
    for pid, ptag, ploc in pillars:
        write_entity(
            pid,
            ptag,
            {"Location": ploc},
            """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [1.2, 5.0, 1.2]
    BevelSize: 0.0
    GenerateCollision: 1
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.75, 0.77, 0.82, 1.0]
    Mesh: ""
    Material: ""
"""
        )

    # 20..21: North Partition Walls
    write_entity(
        970000000000000020,
        "North Partition West",
        {"Location": [13.5, 2.0, 31.0]},
        """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [11.0, 4.0, 0.5]
    BevelSize: 0.0
    GenerateCollision: 1
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.60, 0.64, 0.70, 1.0]
    Mesh: ""
    Material: ""
"""
    )
    write_entity(
        970000000000000021,
        "North Partition East",
        {"Location": [26.5, 2.0, 31.0]},
        """  World::WhiteboxComponent:
    Shape: 0
    Subdivisions: 1
    Steps: 8
    Dimensions: [11.0, 4.0, 0.5]
    BevelSize: 0.0
    GenerateCollision: 1
  World::MeshRendererComponent:
    Primitive: 0
    MeshIndex: 0
    Color: [0.60, 0.64, 0.70, 1.0]
    Mesh: ""
    Material: ""
"""
    )

    print(f"[Entities] Generated 21 level entities in '{ENTITIES_DIR}'")

if __name__ == "__main__":
    build_generic_navmesh()
    build_level_and_entities()
    print("[Done] All generic showcase assets generated successfully.")
