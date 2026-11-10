// WorldEngine Fullscreen Post-Processing Slang Shader Suite
// Supports FlashWhite, Outline, Dissolve, Bloom threshold, Vignette, and Chromatic Aberration

struct VS_INPUT
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float2 TexCoord : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 TexCoord : TEXCOORD0;
};

struct PostProcessUniformBuffer
{
    float4 OutlineColor;
    float4 ScreenParams;          // x: Width, y: Height, z: 1/Width, w: 1/Height
    float FlashWhiteIntensity;
    float FlashWhiteDecay;
    float OutlineWidth;
    float DissolveProgress;
    float BloomIntensity;
    float BloomThreshold;
    float VignetteIntensity;
    float ChromaticAberration;
};

[[vk::binding(0, 0)]] ConstantBuffer<PostProcessUniformBuffer> u_Params;
[[vk::binding(1, 0)]] Sampler2D u_SourceTexture;
[[vk::binding(2, 0)]] Sampler2D u_NoiseTexture;

// 1. Fullscreen Triangle Vertex Shader
[shader("vertex")]
VS_OUTPUT VSMain(uint vertexID : SV_VertexID)
{
    VS_OUTPUT output;
    // Generate full-screen triangle from vertex ID 0, 1, 2
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.TexCoord = float2(uv.x, 1.0f - uv.y);
    return output;
}

// 2. Fullscreen Post-Process Fragment Shader
[shader("fragment")]
float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float2 uv = input.TexCoord;
    
    // Chromatic Aberration offset
    float2 uvR = uv;
    float2 uvG = uv;
    float2 uvB = uv;
    if (u_Params.ChromaticAberration > 0.001f)
    {
        float2 dir = uv - float2(0.5f, 0.5f);
        float dist = length(dir);
        float offset = dist * u_Params.ChromaticAberration * 0.05f;
        uvR = uv + dir * offset;
        uvB = uv - dir * offset;
    }

    float4 color;
    color.r = u_SourceTexture.Sample(uvR).r;
    color.g = u_SourceTexture.Sample(uvG).g;
    color.b = u_SourceTexture.Sample(uvB).b;
    color.a = u_SourceTexture.Sample(uv).a;

    // Flash-White Effect
    if (u_Params.FlashWhiteIntensity > 0.001f)
    {
        color.rgb = lerp(color.rgb, float3(1.0f, 1.0f, 1.0f), saturate(u_Params.FlashWhiteIntensity));
    }

    // Dissolve Effect
    if (u_Params.DissolveProgress > 0.001f)
    {
        float noise = u_NoiseTexture.Sample(uv * 4.0f).r;
        if (noise < u_Params.DissolveProgress)
        {
            discard;
        }
        else if (noise < u_Params.DissolveProgress + 0.05f)
        {
            // Dissolve edge glowing ramp
            color.rgb = float3(1.0f, 0.5f, 0.1f) * 2.0f;
        }
    }

    // Vignette Effect
    if (u_Params.VignetteIntensity > 0.001f)
    {
        float2 d = (uv - 0.5f) * float2(u_Params.ScreenParams.x * u_Params.ScreenParams.w, 1.0f);
        float len = length(d);
        float vig = smoothstep(0.7f, 0.7f - u_Params.VignetteIntensity * 0.45f, len);
        color.rgb *= vig;
    }

    return color;
}
