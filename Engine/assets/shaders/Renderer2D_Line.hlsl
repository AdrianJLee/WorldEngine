cbuffer Camera : register(b0)
{
    float4x4 u_ViewProjection;
};

struct VertexInput
{
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float4 a_Color : COLOR;
    [[vk::location(2)]] int a_EntityID : TEXCOORD1;
};

struct VertexOutput
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float4 v_Color : COLOR;
    [[vk::location(1)]] nointerpolation int v_EntityID : TEXCOORD1;
};

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    
    // 1. 计算顶点位置
    output.Position = mul(u_ViewProjection, float4(input.a_Position, 1.0));
    
    // 2. 透传颜色和 EntityID
    output.v_Color = input.a_Color;
    output.v_EntityID = input.a_EntityID;
    
    return output;
}

struct PixelOutput
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target2;
};

PixelOutput PSMain(VertexOutput input)
{
    PixelOutput output;
    
    // 1. 直接输出颜色
    output.Color = input.v_Color;
    
    // 3. 输出 EntityID
    output.EntityID = input.v_EntityID;
    
    return output;
}
