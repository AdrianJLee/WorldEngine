// WUI RHI 后端:纯 2D UI 批绘制(Slang 单源,双目标:Vulkan / GL SPIR-V)。
// set=0 binding=0: 视口投影 UBO; set=1 binding=1: 组合采样器 Sampler2D。
// 组合类型是 GL_SPIRV 的硬要求(ARB_gl_spirv 不接受 OpTypeSampler);
// Vulkan 侧同一份声明映射成 COMBINED_IMAGE_SAMPLER 描述符(Slang-T2)。
struct VS_INPUT
{
    [[vk::location(0)]] float2 a_Position : POSITION;
    [[vk::location(1)]] float4 a_Color : COLOR;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float4 v_Color : COLOR;
    [[vk::location(1)]] float2 v_TexCoord : TEXCOORD0;
};

struct PS_OUTPUT
{
    float4 Color : SV_Target0;
};

[[vk::binding(0, 0)]] cbuffer Uniforms
{
    float4x4 u_ViewProjection;
};

[[vk::binding(1, 1)]] Sampler2D u_Texture;

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    output.Position = mul(u_ViewProjection, float4(input.a_Position, 0.0f, 1.0f));
    output.v_Color = input.a_Color;
    output.v_TexCoord = input.a_TexCoord;
    return output;
}

PS_OUTPUT PSMain(VS_OUTPUT input)
{
    PS_OUTPUT output;
    // uv.x < -0.5 表示纯色矩形,不走纹理采样。
    if (input.v_TexCoord.x < -0.5f)
        output.Color = input.v_Color;
    else
        output.Color = u_Texture.Sample(input.v_TexCoord) * input.v_Color;
    return output;
}
