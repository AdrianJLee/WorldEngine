// P1b D2b + D3:3D 网格着色器。
//  - set 0 = 全局相机 UBO(u_ViewProjection)
//  - set 1 = 每对象 UBO(u_Model / 材质标量 / u_EntityId)
//            + binding 3 = 骨骼调色板 UBO(u_Bones[128],D5c-3b GPU 蒙皮;不能放 binding 2:
//            GL 后端描述符单元 = binding(忽略 set),binding 2 已被 set0 的灯光 UBO 占用)
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
    // D8b-2:基础色与实体 id 改成**插值量** —— 实例化合批时它们来自 per-instance 属性,
    // 逐物体路径从对象 UBO 填同样的值,两条路径共用同一个 PSMain(像素与拾取不变)。
    // nointerpolation:整物体/整批常量,平直插值既省又避免浮点插值误差(id 要精确整数)。
    [[vk::location(3)]] nointerpolation float4 v_BaseColor : BASECOLOR;
    [[vk::location(4)]] nointerpolation float v_EntityId : ENTITYID;
};

// D8b-2:实例属性(binding 1 = PerInstance;stride 96B,与 C++ Renderer3D::InstanceData 一致)。
// 模型矩阵按**行**上传(glm 的 row0..row3),float4x4(...) 构造按行取值,
// 这样与 cbuffer 路径的 mul(matrix, vector) 语义完全一致。
//
// Slang-T2:实例入口用**一个**合并输入 struct(顶点属性 0..2 + per-instance 3..8)。
// Slang 不会把两个参数各自的 struct 按显式 `[[vk::location]]` 合并成一个接口:
// 第二个参数(实例属性)会被排到 location 6..11 —— 与 C++ 顶点布局 3..8 不符,
// 实例矩阵会读到错位的属性。合并成单参数后 location 逐项等于改造前(dxc 时代)的产出。
struct VS_INSTANCED_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float3 a_Normal : NORMAL;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD0;
    [[vk::location(3)]] float4 i_Row0 : INSTANCE0;
    [[vk::location(4)]] float4 i_Row1 : INSTANCE1;
    [[vk::location(5)]] float4 i_Row2 : INSTANCE2;
    [[vk::location(6)]] float4 i_Row3 : INSTANCE3;
    [[vk::location(7)]] float4 i_Color : INSTANCE4;
    [[vk::location(8)]] float4 i_EntityId : INSTANCE5;
};

// D5c-3b:蒙皮顶点输入(.wmodel 顶点布局 2:标准 32B + joints/weights 32B,stride 64)。
// 关节下标用 float 承载(0..127 精确可表示),与容器/网格一致 —— 不引入整数顶点属性,
// GL 后端的 glVertexArrayAttribFormat 会把整数属性按浮点读(D8b-2 实例化已踩过)。
struct VS_SKINNED_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float3 a_Normal : NORMAL;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD0;
    [[vk::location(3)]] float4 a_Joints : JOINTS;
    [[vk::location(4)]] float4 a_Weights : WEIGHTS;
};

struct PS_OUTPUT
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

// set 0, binding 0:由 SceneRenderer 每帧绑定的全局相机矩阵。
[[vk::binding(0, 0)]] cbuffer CameraUniforms
{
    float4x4 u_ViewProjection;
};

// P1b D4:set 0, binding 2 的灯光系统(方向光 + 点光 + 环境光 + 方向光阴影矩阵)。
// 布局与 C++ 的 LightUniforms(std140,496 字节)逐字段对应,详见 Renderer3D.h。
struct GpuLight
{
    float4 PositionType;      // xyz = 位置(点光) / w = 0 点光 | 1 方向光
    float4 ColorIntensity;    // rgb = 线性色, a = 强度
    float4 DirectionRange;    // 方向光:xyz = 传播方向(已归一化);点光:x = 范围
};

[[vk::binding(2, 0)]] cbuffer LightUniforms
{
    float4x4 u_ShadowViewProjection;
    // x = 启用阴影, y = 深度 bias, z = 贴图边长, w = PCF 半径(纹素)。
    float4 u_ShadowParams;
    // rgb = 环境光线性色, a = 强度(无 AmbientLightComponent 时是 0.25 灰默认值)。
    float4 u_Ambient;
    // x = 方向光数, y = 点光数, z = 深度约定(0 = Vulkan 的 [0,1] 裁剪深度,
    // 1 = OpenGL 的 [-1,1] 裁剪深度 → 深度缓冲存 (z+1)/2)。
    uint4 u_LightCounts;
    GpuLight u_Lights[8];
};

// set 0, binding 3:方向光阴影贴图(D24,点采样 + ClampToEdge;值域 [0,1] 的深度)。
// Slang 组合采样器:一份声明同时喂 Vulkan(COMBINED_IMAGE_SAMPLER)与 GL_SPIRV
// (OpTypeSampledImage —— ARB_gl_spirv 不接受分离的 OpTypeSampler)。
[[vk::binding(3, 0)]] Sampler2D u_ShadowMap;

// set 1, binding 1:每对象数据(Renderer3D 在提交时写入对应帧槽位的 UBO)。
// binding 必须非 0:OpenGL 后端的描述符绑定单元 = binding(忽略 set 索引),
// 用 b0 会与 set0/binding0 的相机 UBO 撞同一个 GL uniform buffer unit → GL 下 3D 全黑。
[[vk::binding(1, 1)]] cbuffer ObjectUniforms
{
    float4x4 u_Model;
    float4 u_BaseColor;
    // D3 材质标量(sRGB 空间颜色;标量用 float4 承载,与 C++ std140 布局严格一致)。
    float4 u_MetallicRoughness;   // x = metallic, y = roughness
    float4 u_Emissive;            // rgb = emissive
    float4 u_Flags;               // x = 有 albedo 贴图, y = 有法线贴图, z = 双面, w = 法线是 BC5 产物(重建 Z)
    // P1b D7-1c:视口点选用的实体 id(写进 SV_Target1);-1 = 不可拾取。
    // 用 int4 而不是 int/int3:标量+短向量在 HLSL 与 std140 下的偏移不一致,
    // GL 的 SPIR-V 环境(std140)不接受这种块布局(旧 GLSL 路径同样拒绝过)。
    int4 u_EntityId;
};

// D5c-3b:set 1, binding 3 = 骨骼调色板(每绘制的 8KB UBO;非蒙皮绘制绑默认调色板)。
// 为什么不是 binding 2(2026-09-19 回归修复):GL 后端绑定单元 = binding(忽略 set),
// 与 set0/binding2 的 LightUniforms 同号,glBindBufferRange 会互相覆盖(实测像素基线失败)。
// GL UBO 单元占用表:0 = 相机、1 = 物体、2 = 灯光、3 = 骨骼;贴图走 GL 纹理单元(独立命名空间)。
// 与 C++ 的 Renderer3D::BoneUniforms(128 × float4x4,std140 8192B)逐字段对应。
// 为什么是 UBO 而不是 SSBO(主 agent 2026-09-19 冻结):GL 后端的 SSBO 描述符绑定支持
// 未经验证,而"每绘制写 UBO"是本项目对象 UBO 已有的路径;128 × 64B 在 UBO 上限内。
// 下标必须**动态**合法:调用方保证 paletteCount ≤ 128,顶点里的关节下标在这里 clamp。
[[vk::binding(3, 1)]] cbuffer BoneUniforms
{
    float4x4 u_Bones[128];
};

// set 2:材质贴图。albedo 以 sRGB 格式创建(硬件解码到线性),
// 法线贴图是线性数据(不解码)。
[[vk::binding(1, 2)]] Sampler2D u_AlbedoTexture;
[[vk::binding(2, 2)]] Sampler2D u_NormalTexture;

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
    output.v_BaseColor = u_BaseColor;
    output.v_EntityId = (float)u_EntityId.x;
    return output;
}

// D8b-2:实例化合批入口(一次绘制画 N 个实例;per-draw 常量仍来自 set1 的对象 UBO)。
VS_OUTPUT VSMainInstanced(VS_INSTANCED_INPUT input)
{
    VS_OUTPUT output;
    const float4x4 model = float4x4(input.i_Row0, input.i_Row1,
        input.i_Row2, input.i_Row3);
    const float4 worldPosition = mul(model, float4(input.a_Position, 1.0f));
    output.Position = mul(u_ViewProjection, worldPosition);
    const float3x3 normalMatrix = (float3x3)transpose((float3x3)model);
    output.v_Normal = normalize(mul(normalMatrix, input.a_Normal));
    output.v_WorldPosition = worldPosition.xyz;
    output.v_TexCoord = input.a_TexCoord;
    output.v_BaseColor = input.i_Color;
    output.v_EntityId = input.i_EntityId.x;
    return output;
}

// D5c-3b:蒙皮的线性混合(4 关节/顶点,与容器布局一致)。
// - 关节下标 clamp 到 [0,127]:越界数据在 GL 上不会报错,不 clamp 会读到 UBO 之外(未定义值);
// - weights 全 0(或全非有限)退化为"不蒙皮":直接用 u_Model。没有这条,坏数据/未赋权顶点
//   会被 (0,0,0) 矩阵打成退化点,整块网格消失 —— 反而更难排查;
// - 法线用调色板矩阵的 3×3 部分混合后**重新归一化**(参考实现口径;非均匀缩放下不精确,
//   与"顶点布局里没有切线/逆转置"的既有约定一致)。
// 注意:变量名不用 linear —— 旧 GLSL 路径里它是插值修饰符关键字(实测);现已走 SPIR-V,保留旧名避免缓存/引用漂移。
float4x4 ComputeSkinPalette(VS_SKINNED_INPUT input)
{
    const int4 joints = (int4)round(input.a_Joints);
    float4x4 blended = (float4x4)0;
    [unroll] for (int index = 0; index < 4; ++index)
    {
        const uint boneIndex = (uint)clamp(joints[index], 0, 127);
        blended += u_Bones[boneIndex] * input.a_Weights[index];
    }
    return blended;
}

// D5c-3b:蒙皮顶点入口(顶点属性 = 布局 2,stride 64)。PSMain 与静态路径共用。
VS_OUTPUT VSMainSkinned(VS_SKINNED_INPUT input)
{
    VS_OUTPUT output;
    const float4x4 palette = ComputeSkinPalette(input);
    const float4 skinnedPosition = mul(palette, float4(input.a_Position, 1.0f));
    const float3 skinnedNormal = mul((float3x3)palette, input.a_Normal);
    const float3 localNormal = dot(skinnedNormal, skinnedNormal) > 1e-12f
        ? normalize(skinnedNormal) : input.a_Normal;
    // weights 全 0(或结果非有限)退化为"不蒙皮":没有这条,坏数据/未赋权顶点会被
    // (0,0,0) 矩阵打成退化点,整块网格消失 —— 反而更难排查。
    const bool hasWeights = any(input.a_Weights != 0.0f) && all(isfinite(skinnedPosition));
    const float4 localPosition = hasWeights ? skinnedPosition : float4(input.a_Position, 1.0f);

    const float4 worldPosition = mul(u_Model, localPosition);
    output.Position = mul(u_ViewProjection, worldPosition);
    const float3x3 normalMatrix = (float3x3)transpose((float3x3)u_Model);
    output.v_Normal = normalize(mul(normalMatrix, localNormal));
    output.v_WorldPosition = worldPosition.xyz;
    output.v_TexCoord = input.a_TexCoord;
    output.v_BaseColor = u_BaseColor;
    output.v_EntityId = (float)u_EntityId.x;
    return output;
}

// 方向光 3×3 PCF:返回 1 = 完全受光,0 = 完全在阴影里。
// 深度空间换算必须区分后端:Vulkan 的裁剪空间 z∈[0,1] 就是深度缓冲值;OpenGL 的
// z∈[-1,1] 会被固定管线映射成 (z+1)/2。由 u_LightCounts.z 在运行时选择,
// 两个后端共用同一份着色器(SceneRenderer 已按后端决定是否补 z 重映射)。
float SampleDirectionalShadow(float3 worldPosition, float3 normal, float3 toLightDirection)
{
    const float4 shadowPosition = mul(u_ShadowViewProjection, float4(worldPosition, 1.0f));
    if (shadowPosition.w <= 0.0f)
        return 1.0f;
    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float depth = (u_LightCounts.z > 0u) ? (projected.z * 0.5f + 0.5f) : projected.z;
    const float2 uv = projected.xy * 0.5f + 0.5f;
    // 阴影体积外不投影(ClampToEdge 采样会返回边缘深度,直接判受光更稳)。
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;
    // 斜率相关 bias:掠射角(dot 小)时加深偏移,避免自阴影条纹。
    const float cosTheta = saturate(dot(normal, toLightDirection));
    const float bias = max(u_ShadowParams.y * (1.0f - cosTheta), u_ShadowParams.y * 0.25f);
    const float2 texel = u_ShadowParams.w / max(u_ShadowParams.z, 1.0f);
    float visible = 0.0f;
    [unroll] for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll] for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            const float sampledDepth = u_ShadowMap.Sample(
                uv + float2(offsetX, offsetY) * texel).r;
            visible += (depth - bias <= sampledDepth) ? 1.0f : 0.0f;
        }
    }
    return visible / 9.0f;
}

PS_OUTPUT PSMain(VS_OUTPUT input)
{
    PS_OUTPUT output;

    // D3 材质:albedo(贴图已由硬件 sRGB 解码,基础色是 sRGB 数值需手动解码)。
    // 注意:场景目标是 UNORM(不是 sRGB 目标),所以这里显式做"线性光照 → 显示空间"的
    // 编码;2D/WUI 通道的颜色本就按显示空间书写,不参与这条管线。
    const float3 baseColorLinear = pow(saturate(input.v_BaseColor.rgb), 2.2f);
    float3 albedo = baseColorLinear;
    if (u_Flags.x > 0.5f)
        albedo *= u_AlbedoTexture.Sample(input.v_TexCoord).rgb;

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
        const float3 sampled = u_NormalTexture.Sample(input.v_TexCoord).xyz * 2.0f - 1.0f;
        // M4-TEX P2b:BC5(RGTC2)只存 X/Y,第三通道恒为 -1;u_Flags.w = 法线贴图来自 BC5 产物
        // (Renderer3D 的 NormalBc5Flag)时按切线空间单位向量重建 Z。w = 0(含全部 RGBA8 源图/
        // 回退路径)保持今天的 xyz 解码逐值不变。约定见 docs/dev/shader-contract.md 法线段。
        const float3 tangentNormal = u_Flags.w > 0.5f
            ? float3(sampled.xy, sqrt(saturate(1.0f - dot(sampled.xy, sampled.xy))))
            : sampled;
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y
            + normal * tangentNormal.z);
    }

    // P1b D4:环境项 + 方向光/点光(Lambert + 以 metallic/roughness 调的简化高光)
    // + 方向光 PCF 阴影(只作用于主方向光的漫反射/高光)。
    //
    // 视线方向固定用 (0,0,1):真实视线方向需要相机位置 uniform,而冻结的 496 字节
    // 灯光布局里没有它的位置 —— 沿用占位实现的"简化高光"口径(高光只做粗糙度反馈),
    // 后续要精确高光时再加相机位置字段。
    const float3 viewDirection = float3(0.0f, 0.0f, 1.0f);
    float3 litColor = albedo * u_Ambient.rgb * u_Ambient.a;

    const uint lightCount = u_LightCounts.x + u_LightCounts.y;
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        const GpuLight light = u_Lights[lightIndex];
        const bool isDirectional = light.PositionType.w > 0.5f;
        float3 toLight;
        float attenuation = 1.0f;
        if (isDirectional)
        {
            // DirectionRange.xyz 是光的传播方向(从光源指向场景),入射方向取反。
            toLight = -normalize(light.DirectionRange.xyz);
        }
        else
        {
            const float3 offset = light.PositionType.xyz - input.v_WorldPosition;
            const float distanceToLight = length(offset);
            toLight = distanceToLight > 1e-5f ? offset / distanceToLight : float3(0.0f, 0.0f, 0.0f);
            // 衰减:saturate(1 - d/range)^2(范围内平滑到 0,范围外不发散)。
            const float falloff = saturate(1.0f - distanceToLight / max(light.DirectionRange.x, 1e-4f));
            attenuation = falloff * falloff;
        }

        const float lambert = saturate(dot(normal, toLight));
        float visibility = 1.0f;
        if (isDirectional && u_ShadowParams.x > 0.5f)
            visibility = SampleDirectionalShadow(input.v_WorldPosition, normal, toLight);

        const float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a * attenuation * visibility;
        const float diffuse = lambert * lerp(1.0f, 0.35f, saturate(u_MetallicRoughness.x));
        const float3 halfVector = normalize(toLight + viewDirection + 1e-5f);
        const float specularPower = lerp(8.0f, 128.0f, saturate(1.0f - u_MetallicRoughness.y));
        const float specular = lambert > 0.0f
            ? pow(saturate(dot(normal, halfVector)), specularPower)
                * lerp(0.04f, 1.0f, saturate(u_MetallicRoughness.x))
            : 0.0f;
        litColor += albedo * radiance * diffuse + radiance * specular;
    }

    const float3 emissive = pow(saturate(u_Emissive.rgb), 2.2f);
    litColor += emissive;
    // 变量名不能叫 linear:旧 GLSL 路径里它是插值修饰符关键字(现已走 SPIR-V),
    // GL 侧着色器编译会直接报 "modifiers must appear before type"(实测)。

    // 线性 → 显示空间(与 2D/WUI 的显示空间书写保持同一最终空间)。
    output.Color = float4(pow(saturate(litColor), 1.0f / 2.2f), input.v_BaseColor.a);
    // 实体 id 由顶点阶段按实例给出(浮点承载,id < 2^24 精确);round 保证 -1(不可拾取)
    // 与正数都还原成精确整数(直接截断会把 -1 变成 0,点选会误命中 0 号实体)。
    output.EntityID = (int)round(input.v_EntityId);
    return output;
}
