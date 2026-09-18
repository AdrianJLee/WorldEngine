// P1b D4:方向光阴影通道(depth-only)。
//  - set 0, binding 2 = 灯光 UBO(u_ShadowViewProjection;SceneRenderer 每帧写入)
//  - set 1, binding 1 = 每对象 UBO(u_Model,与主通道共用 ObjectLayout)
//
// 只写深度:阴影通道的颜色附件是"凑数"用的(OpenGL 的 FBO 完整性要求默认 draw buffer
// 有附件),其 Load/Store 都是 DontCare,从不被采样。
//
// **必须保留片元阶段(空 PSMain)**:GL 侧删掉片元阶段会链接失败;它不写任何 SV_Target,
// 因此不产生颜色写入,也不违反"只写深度"的语义。

struct VS_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
};

struct VS_OUTPUT
{
    float4 Position : SV_Position;
};

// set 0, binding 2:与 Renderer3D_Solid.hlsl 的 cbuffer LightUniforms 同布局
// (C++ 侧 LightUniforms,std140,496 字节)。阴影阶段只读第一个字段。
struct GpuLight
{
    float4 PositionType;
    float4 ColorIntensity;
    float4 DirectionRange;
};

cbuffer LightUniforms : register(b2)
{
    float4x4 u_ShadowViewProjection;
    float4 u_ShadowParams;
    float4 u_Ambient;
    uint4 u_LightCounts;
    GpuLight u_Lights[8];
};

// set 1, binding 1:每对象数据(Renderer3D 提交时写入;这里只用到 u_Model)。
cbuffer ObjectUniforms : register(b1, space1)
{
    float4x4 u_Model;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    const float4 worldPosition = mul(u_Model, float4(input.a_Position, 1.0f));
    output.Position = mul(u_ShadowViewProjection, worldPosition);
    return output;
}

void PSMain(VS_OUTPUT input)
{
    // 空片元阶段:阴影通道只写深度。
}
