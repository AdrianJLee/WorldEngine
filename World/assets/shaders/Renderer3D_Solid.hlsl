// P1b D2b:3D 实心网格着色器(set 0 = 全局相机 UBO,set 1 = 每对象 UBO)。
struct VS_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float3 a_Normal : NORMAL;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 v_Normal : NORMAL;
    [[vk::location(1)]] float3 v_WorldPosition : POSITION1;
};

struct PS_OUTPUT
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

// set 0, binding 0:由 SceneRenderer 每帧绑定的全局相机矩阵。
cbuffer CameraUniforms : register(b0)
{
    float4x4 u_ViewProjection;
};

// set 1, binding 1:每对象数据(Renderer3D 在提交时写入对应帧槽位的 UBO)。
// binding 必须非 0:OpenGL 后端的描述符绑定单元 = binding(忽略 set 索引),
// 用 b0 会与 set0/binding0 的相机 UBO 撞同一个 GL uniform buffer unit → GL 下 3D 全黑。
cbuffer ObjectUniforms : register(b1, space1)
{
    float4x4 u_Model;
    float4 u_BaseColor;
    // P1b D7-1c:视口点选用的实体 id(写进 SV_Target1);-1 = 不可拾取。
    // 用 int4 而不是 int/int3:标量+短向量在 HLSL 与 std140 下的偏移不一致,
    // spirv-cross 会直接拒绝这个块("Buffer block cannot be expressed as std140/std430")。
    int4 u_EntityId;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    const float4 worldPosition = mul(u_Model, float4(input.a_Position, 1.0f));
    output.Position = mul(u_ViewProjection, worldPosition);
    output.v_Normal = normalize(mul((float3x3)u_Model, input.a_Normal));
    output.v_WorldPosition = worldPosition.xyz;
    return output;
}

PS_OUTPUT PSMain(VS_OUTPUT input)
{
    PS_OUTPUT output;
    // 简易 lambert:固定方向光 + 环境项(D3/D4 会替换为材质与光照系统)。
    const float3 lightDirection = normalize(float3(0.35f, -0.7f, 0.6f));
    const float lambert = saturate(dot(normalize(input.v_Normal), -lightDirection));
    output.Color = float4(u_BaseColor.rgb * (0.25f + 0.75f * lambert), u_BaseColor.a);
    output.EntityID = u_EntityId.x;
    return output;
}
