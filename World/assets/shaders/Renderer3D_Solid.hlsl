// P1b D2b + D3:3D 网格着色器。
//  - set 0 = 全局相机 UBO(u_ViewProjection)
//  - set 1 = 每对象 UBO(u_Model / 材质标量 / u_EntityId)
//  - set 2 = 材质贴图(Albedo = sRGB, Normal = 线性)
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
    [[vk::location(2)]] float2 v_TexCoord : TEXCOORD0;
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
    // D3 材质标量(sRGB 空间颜色;标量用 float4 承载,与 C++ std140 布局严格一致)。
    float4 u_MetallicRoughness;   // x = metallic, y = roughness
    float4 u_Emissive;            // rgb = emissive
    float4 u_Flags;               // x = 有 albedo 贴图, y = 有法线贴图, z = 双面
    // P1b D7-1c:视口点选用的实体 id(写进 SV_Target1);-1 = 不可拾取。
    // 用 int4 而不是 int/int3:标量+短向量在 HLSL 与 std140 下的偏移不一致,
    // spirv-cross 会直接拒绝这个块("Buffer block cannot be expressed as std140/std430")。
    int4 u_EntityId;
};

// set 2:材质贴图。albedo 以 sRGB 格式创建(硬件解码到线性),
// 法线贴图是线性数据(不解码)。
[[vk::combinedImageSampler]] Texture2D u_AlbedoTexture : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState u_AlbedoSampler : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D u_NormalTexture : register(t2, space2);
[[vk::combinedImageSampler]] SamplerState u_NormalSampler : register(s2, space2);

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    const float4 worldPosition = mul(u_Model, float4(input.a_Position, 1.0f));
    output.Position = mul(u_ViewProjection, worldPosition);
    // 非均匀缩放时法线要用逆转置矩阵,否则光照方向会偏。
    const float3x3 normalMatrix = (float3x3)transpose((float3x3)u_Model);
    output.v_Normal = normalize(mul(normalMatrix, input.a_Normal));
    output.v_WorldPosition = worldPosition.xyz;
    output.v_TexCoord = input.a_TexCoord;
    return output;
}

PS_OUTPUT PSMain(VS_OUTPUT input)
{
    PS_OUTPUT output;

    // D3 材质:albedo(贴图已由硬件 sRGB 解码,基础色是 sRGB 数值需手动解码)。
    // 注意:场景目标是 UNORM(不是 sRGB 目标),所以这里显式做"线性光照 → 显示空间"的
    // 编码;2D/WUI 通道的颜色本就按显示空间书写,不参与这条管线。
    const float3 baseColorLinear = pow(saturate(u_BaseColor.rgb), 2.2f);
    float3 albedo = baseColorLinear;
    if (u_Flags.x > 0.5f)
        albedo *= u_AlbedoTexture.Sample(u_AlbedoSampler, input.v_TexCoord).rgb;

    float3 normal = normalize(input.v_Normal);
    if (u_Flags.y > 0.5f)
    {
        // 切线空间由位置导数构造:避免在 D3 阶段给顶点布局强加切线属性
        // (导入器提供切线后(D5)替换这里)。
        const float3 dpdx = ddx(input.v_WorldPosition);
        const float3 dpdy = ddy(input.v_WorldPosition);
        const float2 duvdx = ddx(input.v_TexCoord);
        const float2 duvdy = ddy(input.v_TexCoord);
        const float3 tangent = normalize(dpdx * duvdy.y - dpdy * duvdx.y + 1e-6f);
        const float3 bitangent = normalize(cross(normal, tangent));
        const float3 sampled = u_NormalTexture.Sample(u_NormalSampler, input.v_TexCoord).xyz * 2.0f - 1.0f;
        normal = normalize(tangent * sampled.x + bitangent * sampled.y + normal * sampled.z);
    }

    // 占位光照(固定方向光 + 环境项):D4 会替换为真正的光照系统。
    const float3 lightDirection = normalize(float3(0.35f, -0.7f, 0.6f));
    const float lambert = saturate(dot(normal, -lightDirection));
    // 粗糙度只做最轻的视觉反馈(高光收窄),避免在 D4 之前引入半成品 BRDF。
    const float3 halfVector = normalize(-lightDirection + float3(0.0f, 0.0f, 1.0f));
    const float specularPower = lerp(8.0f, 128.0f, saturate(1.0f - u_MetallicRoughness.y));
    const float specular = pow(saturate(dot(normal, halfVector)), specularPower)
        * lerp(0.04f, 1.0f, saturate(u_MetallicRoughness.x));

    const float3 ambient = albedo * 0.25f;
    const float3 diffuse = albedo * lambert * lerp(1.0f, 0.35f, saturate(u_MetallicRoughness.x));
    const float3 emissive = pow(saturate(u_Emissive.rgb), 2.2f);
    // 变量名不能叫 linear:spirv-cross 生成的 GLSL 里 "linear" 是插值修饰符关键字,
    // GL 侧着色器编译会直接报 "modifiers must appear before type"(实测)。
    const float3 litColor = ambient + diffuse + specular + emissive;

    // 线性 → 显示空间(与 2D/WUI 的显示空间书写保持同一最终空间)。
    output.Color = float4(pow(saturate(litColor), 1.0f / 2.2f), u_BaseColor.a);
    output.EntityID = u_EntityId.x;
    return output;
}
