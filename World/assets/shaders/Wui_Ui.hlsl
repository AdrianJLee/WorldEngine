// WUI RHI 后端:纯 2D UI 批绘制。
// set=0 binding=0: 视口投影 UBO; set=1 binding=1: 纹理 + 采样器。
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

cbuffer Uniforms : register(b0)
{
    float4x4 u_ViewProjection;
};

Texture2D u_Texture : register(t1);
SamplerState u_Sampler : register(s1);

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
    output.Color = u_Texture.Sample(u_Sampler, input.v_TexCoord) * input.v_Color;
    return output;
}
