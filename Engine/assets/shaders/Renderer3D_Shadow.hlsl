// P1b D4:方向光阴影通道(depth-only)。
//  - set 0, binding 2 = 灯光 UBO(u_ShadowViewProjection;SceneRenderer 每帧写入)
//  - set 1, binding 1 = 每对象 UBO(u_Model,与主通道共用 ObjectLayout)
//  - set 1, binding 3 = 骨骼调色板 UBO(u_Bones[128],D5c-3b;与主通道共用 ObjectLayout)
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

// D5c-3b:set 1, binding 3 = 骨骼调色板(与 Renderer3D_Solid.hlsl 同布局)。
// 用 3 而不是 2:GL 后端绑定单元 = binding(忽略 set),binding 2 是 set0 的灯光 UBO。
// 蒙皮投影者必须在这里同样做混合,否则影子留在**绑定姿态**(与网格错位)。
cbuffer BoneUniforms : register(b3, space1)
{
    float4x4 u_Bones[128];
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    const float4 worldPosition = mul(u_Model, float4(input.a_Position, 1.0f));
    output.Position = mul(u_ShadowViewProjection, worldPosition);
    return output;
}

// D8b-2:实例化合批入口。属性布局与 Renderer3D_Solid.hlsl 的 VS_INSTANCE_INPUT 一致
// (同一个实例缓冲,只是这里只用到模型矩阵 4 行;颜色/实体 id 由主通道的着色器使用)。
struct VS_INSTANCE_INPUT
{
    [[vk::location(3)]] float4 i_Row0 : INSTANCE0;
    [[vk::location(4)]] float4 i_Row1 : INSTANCE1;
    [[vk::location(5)]] float4 i_Row2 : INSTANCE2;
    [[vk::location(6)]] float4 i_Row3 : INSTANCE3;
};

VS_OUTPUT VSMainInstanced(VS_INPUT input, VS_INSTANCE_INPUT instance)
{
    VS_OUTPUT output;
    const float4x4 model = float4x4(instance.i_Row0, instance.i_Row1,
        instance.i_Row2, instance.i_Row3);
    const float4 worldPosition = mul(model, float4(input.a_Position, 1.0f));
    output.Position = mul(u_ShadowViewProjection, worldPosition);
    return output;
}

// D5c-3b:蒙皮顶点入口(顶点属性 = 网格布局 2,stride 64)。
// 只关心位置:与主通道 VSMainSkinned 用同一套"下标 clamp + weights 全 0 退化为 u_Model"口径,
// 保证影子与网格落在同一姿态。
struct VS_SKINNED_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(3)]] float4 a_Joints : JOINTS;
    [[vk::location(4)]] float4 a_Weights : WEIGHTS;
};

VS_OUTPUT VSMainSkinned(VS_SKINNED_INPUT input)
{
    VS_OUTPUT output;
    const int4 joints = (int4)round(input.a_Joints);
    float4x4 blended = (float4x4)0;
    [unroll] for (int index = 0; index < 4; ++index)
    {
        const uint boneIndex = (uint)clamp(joints[index], 0, 127);
        blended += u_Bones[boneIndex] * input.a_Weights[index];
    }
    const float4 skinnedPosition = mul(blended, float4(input.a_Position, 1.0f));
    const bool hasWeights = any(input.a_Weights != 0.0f) && all(isfinite(skinnedPosition));
    const float4 localPosition = hasWeights ? skinnedPosition : float4(input.a_Position, 1.0f);
    output.Position = mul(u_ShadowViewProjection, mul(u_Model, localPosition));
    return output;
}

void PSMain(VS_OUTPUT input)
{
    // 空片元阶段:阴影通道只写深度。
}
