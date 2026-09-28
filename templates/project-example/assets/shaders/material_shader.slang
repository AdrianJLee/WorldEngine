// Slang material shader (.slang) -- HLSL syntax with Slang strict rules.
//
// Hard rules (the old lax HLSL forms are errors now):
//   1. Strict types: no implicit width conversion.
//      `float3 v = color4;` is an error -> truncate explicitly: `float3 v = color4.xyz;`
//      (`float3v += color4;` is an error too -> `float3v += color4.xyz;`).
//   2. A `//! param Texture2D` declaration becomes a combined sampler `Sampler2D <name>`;
//      sample it as `Albedo.Sample(uv)` (uv = input.UV) -- there is no sampler argument.
//   3. Resources use explicit `[[vk::binding(N, S)]]`; `register(...)` is not supported.
//
// Contract: docs/dev/shader-contract.md

// Material parameters are declared in the file itself; the editor reads these
// annotations to build the parameter panel (values default from the shader).
// Syntax: //! param <type> <name> = <default> [min,max] unit("x") group("G") label("L")
// Types (case sensitive): Float Vec2 Vec3 Vec4 Color Int Bool Texture2D.
// [min,max] only applies to Float / Int; values use the material value dialect
// (0.25 / 1, 0.5 / true / textures/icon.png).

//! param Float     roughness   = 0.35 [0.02,1] group("Surface") label("Roughness")
//! param Color     baseColor   = 0.85, 0.72, 0.55, 1 group("Surface") label("Base Colour")
//! param Texture2D albedo      = "" group("Maps") label("Albedo")
//! param Bool      doubleSided = false group("Geometry") label("Double Sided")
//! param Int       uvChannel   = 0 [0,3] group("Maps") label("UV Channel")

// Engine standard surface. Change only the fields this material needs.
// MaterialInputs is filled by the engine; Surface defaults come from the
// engine's standard shader and stay complete even if you change nothing.
Surface Evaluate(MaterialInputs input)
{
    Surface surface = MakeDefaultSurface();
    // Examples:
    // surface.BaseColor = float3(1.0f, 0.35f, 0.2f);
    // surface.Roughness = 0.25f;
    // surface.Normal = float3(0.0f, 0.0f, 1.0f);
    return surface;
}
