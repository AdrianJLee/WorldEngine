// ==============================================================================
// WorldEngine 2.0 - WorldVFX Industrial Particle System
// Shader: ParticleCompute.slang
//
// Description:
//   High-throughput GPU Compute simulation shader for millions of particles.
//   Supports double-buffered ping-pong SSBO, 3D divergence-free curl noise,
//   gravity, exponential air drag, vortex fields, lifetime decay, and
//   zero-bubble compact index generation for indirect draw calls.
//
// Targets:
//   - Vulkan SPIR-V 1.3+ (-profile cs_6_0)
//   - OpenGL 4.6 Core GL_SPIRV (-profile cs_5_0+spirv_1_0, -fvk-use-gl-layout)
// ==============================================================================

// ------------------------------------------------------------------------------
// 1. Particle Data Structures (64-byte aligned / 16-byte packed)
// ------------------------------------------------------------------------------

static const uint PARTICLE_FLAG_ACTIVE   = 1u << 0; // Bit 0: Particle is currently active
static const uint PARTICLE_FLAG_COLLIDED = 1u << 1; // Bit 1: Particle suffered collision

struct GpuParticle
{
    float3 Position;
    float Life;
    float3 Velocity;
    float MaxLife;
    float4 Color;
    float2 Size;
    float Rotation;
    float AngularVelocity;
    uint SubUVFrame;
    uint Flags; // bit 0: Active, bit 1: Collided
    uint EmitterIndex;
    uint Padding;
};

// ------------------------------------------------------------------------------
// 2. Resource Bindings
//    Explicit [[vk::binding(binding, set)]] ensures identical descriptor units
//    across both Vulkan and OpenGL ARB_gl_spirv backends.
// ------------------------------------------------------------------------------

[[vk::binding(0, 0)]] RWStructuredBuffer<GpuParticle> InParticles  : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<GpuParticle> OutParticles : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>        AliveIndices : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>        IndirectArgs : register(u3);

// Simulation Uniform Buffer (128 bytes, std140/std430 aligned)
[[vk::binding(4, 0)]] cbuffer SimulationParams : register(b0)
{
    float3 u_Gravity;             // Gravity vector (e.g. [0, -9.8, 0])
    float  u_Dt;                  // Delta time in seconds

    float  u_LinearDrag;          // Air drag coefficient
    float  u_TurbulenceStrength;  // Strength of 3D curl noise turbulence
    float  u_TurbulenceFrequency; // Spatial frequency for noise field
    float  u_Time;                // Global scene time

    float3 u_VortexAxis;          // Normalized axis of rotational vortex
    float  u_VortexStrength;      // Angular vortex velocity strength

    float3 u_VortexCenter;        // Origin of vortex field
    float  u_Padding0;

    float4 u_ColorStart;          // Particle color at birth (t = 0)
    float4 u_ColorEnd;            // Particle color at death (t = 1)

    float  u_SizeStart;           // Particle billboard size at birth
    float  u_SizeEnd;             // Particle billboard size at death
    float  u_EmissiveBoost;       // HDR multiplier for emissive color
    float  u_SubUVFrameRate;      // Sub-UV texture flipbook animation rate

    uint   u_MaxParticles;        // Total capacity of particle storage
    uint   u_TotalActiveInput;    // Active particle count submitted from CPU/previous pass
    uint   u_SubUVTotalFrames;    // Total flipbook frames (e.g. 16 for 4x4 atlas)
    uint   u_TurbulenceOctaves;   // Octave count for FBM curl noise [1, 4]
};

// ------------------------------------------------------------------------------
// 3. Mathematical Noise & Divergence-Free 3D Curl Noise Field
// ------------------------------------------------------------------------------

// High-quality hash without trigonometric precision degradation
float3 Hash3D(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx) * 2.0 - 1.0;
}

// 3D Quintic Hermite Gradient Noise
float GradientNoise3D(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);

    // Quintic smootherstep curve: 6t^5 - 15t^4 + 10t^3
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float n000 = dot(Hash3D(i + float3(0.0, 0.0, 0.0)), f - float3(0.0, 0.0, 0.0));
    float n100 = dot(Hash3D(i + float3(1.0, 0.0, 0.0)), f - float3(1.0, 0.0, 0.0));
    float n010 = dot(Hash3D(i + float3(0.0, 1.0, 0.0)), f - float3(0.0, 1.0, 0.0));
    float n110 = dot(Hash3D(i + float3(1.0, 1.0, 0.0)), f - float3(1.0, 1.0, 0.0));
    float n001 = dot(Hash3D(i + float3(0.0, 0.0, 1.0)), f - float3(0.0, 0.0, 1.0));
    float n101 = dot(Hash3D(i + float3(1.0, 0.0, 1.0)), f - float3(1.0, 0.0, 1.0));
    float n011 = dot(Hash3D(i + float3(0.0, 1.0, 1.0)), f - float3(0.0, 1.0, 1.0));
    float n111 = dot(Hash3D(i + float3(1.0, 1.0, 1.0)), f - float3(1.0, 1.0, 1.0));

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);

    float nxy0 = lerp(nx00, nx10, u.y);
    float nxy1 = lerp(nx01, nx11, u.y);

    return lerp(nxy0, nxy1, u.z);
}

// 3D Vector Potential field Psi(x, y, z)
float3 VectorPotential(float3 p)
{
    float3 p0 = p;
    float3 p1 = p + float3(31.416, -47.853, 12.987);
    float3 p2 = p + float3(-53.214, 18.765, 83.123);
    return float3(GradientNoise3D(p0), GradientNoise3D(p1), GradientNoise3D(p2));
}

// Multi-octave Fractal Brownian Motion (FBM) vector potential
float3 VectorPotentialFBM(float3 p, uint octaves)
{
    float3 potential = float3(0.0, 0.0, 0.0);
    float amplitude = 1.0;
    float frequency = 1.0;
    uint maxOctaves = clamp(octaves, 1u, 4u);
    for (uint i = 0u; i < maxOctaves; ++i)
    {
        potential += VectorPotential(p * frequency) * amplitude;
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return potential;
}

// 3D Curl Noise: v = curl(Psi) = del x Psi
// Guaranteed divergence-free: div(curl(Psi)) == 0 (incompressible fluid motion)
float3 ComputeCurlNoise(float3 p, float frequency, uint octaves)
{
    const float eps = 0.01;
    float3 pos = p * frequency;

    float3 p_dx_p = VectorPotentialFBM(pos + float3(eps, 0.0, 0.0), octaves);
    float3 p_dx_m = VectorPotentialFBM(pos - float3(eps, 0.0, 0.0), octaves);
    float3 p_dy_p = VectorPotentialFBM(pos + float3(0.0, eps, 0.0), octaves);
    float3 p_dy_m = VectorPotentialFBM(pos - float3(0.0, eps, 0.0), octaves);
    float3 p_dz_p = VectorPotentialFBM(pos + float3(0.0, 0.0, eps), octaves);
    float3 p_dz_m = VectorPotentialFBM(pos - float3(0.0, 0.0, eps), octaves);

    float inv2eps = 0.5 / eps;
    float dpsi_z_dy = (p_dy_p.z - p_dy_m.z) * inv2eps;
    float dpsi_y_dz = (p_dz_p.y - p_dz_m.y) * inv2eps;
    float dpsi_x_dz = (p_dz_p.x - p_dz_m.x) * inv2eps;
    float dpsi_z_dx = (p_dx_p.z - p_dx_m.z) * inv2eps;
    float dpsi_y_dx = (p_dy_p.x - p_dy_m.x) * inv2eps;
    float dpsi_x_dy = (p_dx_p.y - p_dx_m.y) * inv2eps;

    return float3(
        dpsi_z_dy - dpsi_y_dz,
        dpsi_x_dz - dpsi_z_dx,
        dpsi_y_dx - dpsi_x_dy
    );
}

// ------------------------------------------------------------------------------
// 4. Compute Kernels
// ------------------------------------------------------------------------------

// Initialization entry point to reset indirect argument counter on GPU if required
[shader("compute")]
[numthreads(1, 1, 1)]
void CSInit(uint3 id : SV_DispatchThreadID)
{
    if (id.x == 0u)
    {
        IndirectArgs[0] = 4u; // VertexCount per billboard quad
        IndirectArgs[1] = 0u; // InstanceCount (alive particles counter)
        IndirectArgs[2] = 0u; // FirstVertex
        IndirectArgs[3] = 0u; // FirstInstance
    }
}

// Primary simulation and compaction kernel
[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;

    // Thread 0 initializes constant indirect draw arguments:
    // IndirectArgs[0] = 4 (VertexCount per billboard quad);
    // IndirectArgs[2] = 0 (FirstVertex);
    // IndirectArgs[3] = 0 (FirstInstance);
    // Note: IndirectArgs[1] (AliveCount) is dynamically accumulated via InterlockedAdd
    // or pre-cleared by CPU / CSInit.
    if (index == 0u)
    {
        IndirectArgs[0] = 4u;
        IndirectArgs[2] = 0u;
        IndirectArgs[3] = 0u;
    }

    if (index >= u_MaxParticles)
    {
        return;
    }

    GpuParticle p = InParticles[index];

    // Check if particle is currently active
    if ((p.Flags & PARTICLE_FLAG_ACTIVE) == 0u)
    {
        OutParticles[index] = p;
        return;
    }

    // Advance particle lifetime
    p.Life -= u_Dt;
    if (p.Life <= 0.0)
    {
        p.Life = 0.0;
        p.Flags &= ~PARTICLE_FLAG_ACTIVE;
        OutParticles[index] = p;
        return;
    }

    // Normalized age in [0, 1] (0.0 at birth, 1.0 at expiry)
    float safeMaxLife = max(p.MaxLife, 0.0001);
    float age = 1.0 - clamp(p.Life / safeMaxLife, 0.0, 1.0);

    // Apply linear air drag: v(t + dt) = v(t) * exp(-drag * dt)
    float dragFactor = exp(-max(u_LinearDrag, 0.0) * u_Dt);
    p.Velocity *= dragFactor;

    // Apply gravity acceleration
    p.Velocity += u_Gravity * u_Dt;

    // 3D Curl Noise fluid turbulence (divergence-free solenoidal field)
    if (u_TurbulenceStrength > 0.0)
    {
        float3 curlVelocity = ComputeCurlNoise(p.Position, u_TurbulenceFrequency, u_TurbulenceOctaves);
        p.Velocity += curlVelocity * (u_TurbulenceStrength * u_Dt);
    }

    // Vortex field acceleration (rotational force around vortex axis)
    if (abs(u_VortexStrength) > 0.0)
    {
        float3 r = p.Position - u_VortexCenter;
        float3 tangent = cross(u_VortexAxis, r);
        float dist = length(r);
        if (dist > 0.0001)
        {
            p.Velocity += normalize(tangent) * (u_VortexStrength * u_Dt);
        }
    }

    // Position integration
    p.Position += p.Velocity * u_Dt;

    // Angular velocity and rotation
    p.Rotation += p.AngularVelocity * u_Dt;

    // Color progression over life
    p.Color = lerp(u_ColorStart, u_ColorEnd, age);
    p.Color.rgb *= u_EmissiveBoost;

    // Size progression over life
    float currentSize = lerp(u_SizeStart, u_SizeEnd, age);
    p.Size = float2(currentSize, currentSize);

    // SubUV flipbook animation playback
    if (u_SubUVTotalFrames > 1u)
    {
        if (u_SubUVFrameRate > 0.0)
        {
            float elapsed = max(p.MaxLife - p.Life, 0.0);
            p.SubUVFrame = uint(floor(elapsed * u_SubUVFrameRate)) % u_SubUVTotalFrames;
        }
        else
        {
            p.SubUVFrame = uint(floor(age * float(u_SubUVTotalFrames))) % u_SubUVTotalFrames;
        }
    }

    // Write updated particle state to output buffer
    OutParticles[index] = p;

    // Compact alive particle index for bubble-free indirect rendering
    uint aliveSlot;
    InterlockedAdd(IndirectArgs[1], 1u, aliveSlot);
    AliveIndices[aliveSlot] = index;
}
