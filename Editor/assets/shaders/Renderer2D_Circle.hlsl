// 矩阵缓冲区：绑定到 set=0, binding=0 (由编译器自动分配或显式指定)
cbuffer Uniforms : register(b0)
{
    float4x4 u_ViewProjection;
};

struct VertexInput
{
    [[vk::location(0)]]float3 WorldPosition : POSITION;
    [[vk::location(1)]]float3 LocalPosition : TEXCOORD0;
    [[vk::location(2)]]float4 Color : COLOR;
    [[vk::location(3)]]float Thickness : TEXCOORD1;
    [[vk::location(4)]]float Fade : TEXCOORD2;
    [[vk::location(5)]]int EntityID : TEXCOORD3;
};

struct VertexOutput
{

    [[vk::location(0)]] float3 LocalPosition : TEXCOORD0;
    [[vk::location(1)]] float4 Color : COLOR;
    [[vk::location(2)]] float Thickness : TEXCOORD1;
    [[vk::location(3)]] float Fade : TEXCOORD2;
    [[vk::location(4)]] nointerpolation int EntityID : TEXCOORD3;
    
        // SV_Position 是内置变量，不需要 location，其余参数从 0 开始以供 PS 匹配
    float4 ClipPosition : SV_Position;
};

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    
    // 透传所有的片段需要的变量
    output.LocalPosition = input.LocalPosition;
    output.Color = input.Color;
    output.Thickness = input.Thickness;
    output.Fade = input.Fade;
    output.EntityID = input.EntityID;
    
    // 顶点位置计算
    output.ClipPosition = mul(u_ViewProjection, float4(input.WorldPosition, 1.0));
    
    return output;
}

struct PixelOutput
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

PixelOutput PSMain(VertexOutput input)
{
    // 1. 计算当前点到圆心的距离
    float distance = length(input.LocalPosition.xy);
    
    float fade = max(input.Fade, 0.0001);
    
    // 2. 计算外边缘的平滑抗锯齿
    float circle = 1.0 - smoothstep(1.0 - fade, 1.0, distance);
    
    // 3. 处理空心圆 (Thickness)
    // 只有 Thickness 小于 1.0 才去挖空内部
    // 我们在这个判断加上很小的容差 epsilon，以防止缩放引起的浮点截断导致条件穿透
    if (input.Thickness < 0.999)
    {
        float innerRadius = 1.0 - input.Thickness;
        // 计算内部边缘带来的削减
        circle *= smoothstep(innerRadius, innerRadius + fade, distance);
    }
    
    // 4. 裁剪
    if (circle == 0.0)
        discard;

    PixelOutput output;
    
    // 5. 应用颜色并混合 Alpha
    output.Color = input.Color;
    output.Color.a *= circle;
    
    output.EntityID = input.EntityID;
    return output;
}