// ==========================================
// 1. 结构体定义 (作为阶段间的接口)
// ==========================================

// 顶点输入：对应 Vulkan 的 Vertex Input Binding/Attribute
struct VS_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float4 a_Color : COLOR;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD0;
    [[vk::location(3)]] float a_TexIndex : TEXCOORD1;
    [[vk::location(4)]] float a_TilingFactor : TILING;
    [[vk::location(5)]] int a_EntityID : ENTITY_ID;
    [[vk::location(6)]] float a_NormalTexIndex : TEXCOORD2;
    [[vk::location(7)]] float a_SortingLayer : TEXCOORD3;
};

// 顶点输出 / 像素输入：数据在此结构体中进行插值
struct VS_OUTPUT
{
    // SV_Position 是内建语义，对应 GLSL 的 gl_Position
    float4 Position : SV_Position;
    
    [[vk::location(0)]] float4 v_Color : COLOR;
    [[vk::location(1)]] float2 v_TexCoord : TEXCOORD0;
    // nointerpolation 确保浮点插值不产生向下微量截断(避免 0.999999 变为 0 槽位的白色纹理而导致噪点/撕裂条纹)
    [[vk::location(2)]] nointerpolation float v_TexIndex : TEXCOORD1;
    [[vk::location(3)]] nointerpolation float v_TilingFactor : TILING;
    // nointerpolation 对应 GLSL 的 flat，防止整数被插值
    [[vk::location(4)]] nointerpolation int v_EntityID : ENTITY_ID;
    [[vk::location(5)]] float3 v_WorldPos : TEXCOORD2;
    [[vk::location(6)]] nointerpolation float v_NormalTexIndex : TEXCOORD3;
    [[vk::location(7)]] nointerpolation float v_SortingLayer : TEXCOORD4;
};

// 像素输出：对应 Vulkan 的 Color Attachments
struct PS_OUTPUT
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

// ==========================================
// 2. 资源绑定 (Uniforms & Textures)
// ==========================================

// 矩阵缓冲区:显式 set=0, binding=0(Slang 单源,双目标:Vulkan / GL SPIR-V)。
[[vk::binding(0, 0)]] cbuffer Uniforms
{
    float4x4 u_ViewProjection;
};

// 纹理数组:组合采样器(Sampler2D)数组,set=1 / binding=1,32 个槽位。
// GL_SPIRV 只接受组合形态(OpTypeSampledImage);Vulkan 侧是
// COMBINED_IMAGE_SAMPLER 描述符数组 —— 两端共用这一份声明(Slang-T2)。
[[vk::binding(1, 1)]] Sampler2D u_Textures[32];

struct PointLight2DData
{
    float4 PositionAndRadii;     // xy = worldPos, z = InnerRadius, w = OuterRadius
    float4 ColorAndIntensity;    // rgb = Color, w = Intensity
    float4 DirectionAndAngles;   // xy = forwardDir, z = cos(Inner*0.5), w = cos(Outer*0.5)
    float4 Params;               // x = FalloffIntensity, y = NormalMapZ, z = CastShadows, w = TargetSortingLayer
};

struct ShadowSegment2DData
{
    float4 Endpoints;            // xy = A, zw = B
    float4 Params;               // x = ShadowIntensity, y = EntityID, z = SelfShadows, w = ExtrusionLength
};

[[vk::binding(4, 1)]] cbuffer Light2DUniforms
{
    float4 u_GlobalAmbient;      // rgb = global ambient * intensity
    float4 u_GlobalDirection;    // xy = dir, z = DirectionIntensity, w = LightingEnabled (0 or 1)
    int4   u_LightCounts;        // x = PointLightCount, y = ShadowSegmentCount, z = GlobalAffectAllLayers, w = GlobalTargetSortingLayer
    PointLight2DData u_PointLights[16];
    ShadowSegment2DData u_ShadowSegments[32];
};

// ==========================================
// 3. 顶点着色器 (Vertex Shader)
// ==========================================

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;

    // 传递属性
    output.v_Color = input.a_Color;
    output.v_TexCoord = input.a_TexCoord;
    output.v_TexIndex = input.a_TexIndex;
    output.v_TilingFactor = input.a_TilingFactor;
    output.v_EntityID = input.a_EntityID;
    output.v_WorldPos = input.a_Position;
    output.v_NormalTexIndex = input.a_NormalTexIndex;
    output.v_SortingLayer = input.a_SortingLayer;

    // 坐标变换 (注意 mul 顺序：矩阵在前为列主序运算)
    output.Position = mul(u_ViewProjection, float4(input.a_Position, 1.0f));

    return output;
}

// ==========================================
// 4. 像素着色器 (Pixel Shader)
// ==========================================

bool SegmentsIntersect2D(float2 p0, float2 p1, float2 q0, float2 q1)
{
    float2 r = p1 - p0;
    float2 s = q1 - q0;
    float rxs = r.x * s.y - r.y * s.x;
    if (abs(rxs) <= 1e-6f)
        return false;
    float2 qp = q0 - p0;
    float t = (qp.x * s.y - qp.y * s.x) / rxs;
    float u = (qp.x * r.y - qp.y * r.x) / rxs;
    return (t > 0.001f && t < 0.999f && u >= 0.0f && u <= 1.0f);
}

PS_OUTPUT PSMain(VS_OUTPUT input)
{
    PS_OUTPUT output;

    // 采样纹理：使用纹理对象的 Sample 方法
    // 增加 + 0.5f 解决插值与浮点精度丢失引发的向下截断到槽位 0(白色纹理)导致的噪点与条纹
    uint texSlot = clamp(uint(input.v_TexIndex + 0.5f), 0u, 31u);
    float4 texColor = u_Textures[texSlot].Sample(input.v_TexCoord * input.v_TilingFactor);
    
    // 基础颜色计算
    float4 finalColor = texColor * input.v_Color;
    if (finalColor.a <= 0.001f)
        discard;

    // 2D 专用光照、法线贴图凹凸与 2D 阴影遮挡计算
    if (u_GlobalDirection.w > 0.5f && input.v_SortingLayer > -50000.0f)
    {
        bool hasNormalMap = (input.v_NormalTexIndex >= 0.5f);
        float3 N = float3(0.0f, 0.0f, 1.0f);
        if (hasNormalMap)
        {
            uint normalSlot = clamp(uint(input.v_NormalTexIndex + 0.5f), 0u, 31u);
            float3 nSample = u_Textures[normalSlot].Sample(input.v_TexCoord * input.v_TilingFactor).xyz * 2.0f - 1.0f;
            N = normalize(float3(nSample.xy, max(nSample.z, 0.1f)));
        }

        float3 totalLight = float3(0.0f, 0.0f, 0.0f);
        int sortLayerInt = int(round(input.v_SortingLayer));
        bool globalAffects = (u_LightCounts.z != 0) || (u_LightCounts.w == sortLayerInt);
        if (globalAffects)
        {
            float3 globalTerm = u_GlobalAmbient.rgb;
            if (hasNormalMap && u_GlobalDirection.z > 0.001f)
            {
                float3 globalDir3D = normalize(float3(-u_GlobalDirection.xy, 0.6f));
                float ndotl = max(dot(N, globalDir3D), 0.0f);
                globalTerm = lerp(globalTerm, globalTerm * (0.35f + 0.95f * ndotl), saturate(u_GlobalDirection.z));
            }
            totalLight += globalTerm;
        }

        int lightCount = clamp(u_LightCounts.x, 0, 16);
        int segCount = clamp(u_LightCounts.y, 0, 32);
        float2 fragPos2D = input.v_WorldPos.xy;

        for (int i = 0; i < lightCount; ++i)
        {
            PointLight2DData light = u_PointLights[i];
            float targetLayer = light.Params.w;
            if (targetLayer < 50000.0f && abs(input.v_SortingLayer - targetLayer) > 0.5f)
                continue;

            float2 lightPos = light.PositionAndRadii.xy;
            float innerRadius = light.PositionAndRadii.z;
            float outerRadius = light.PositionAndRadii.w;

            float2 toLight = lightPos - fragPos2D;
            float dist = length(toLight);
            if (dist >= outerRadius)
                continue;

            float normDist = saturate((dist - innerRadius) / max(outerRadius - innerRadius, 0.0001f));
            float falloffExp = lerp(1.0f, 3.0f, saturate(light.Params.x));
            float radialAtten = (dist <= innerRadius) ? 1.0f : pow(1.0f - normDist, falloffExp);

            float spotAtten = 1.0f;
            float cosInner = light.DirectionAndAngles.z;
            float cosOuter = light.DirectionAndAngles.w;
            if (cosOuter > -0.999f && dist > 0.0001f)
            {
                float2 lightToFragDir = -toLight / dist;
                float cosTheta = dot(lightToFragDir, light.DirectionAndAngles.xy);
                spotAtten = saturate((cosTheta - cosOuter) / max(cosInner - cosOuter, 0.0001f));
            }
            if (spotAtten <= 0.0001f)
                continue;

            float shadowVisibility = 1.0f;
            if (light.Params.z > 0.5f && segCount > 0)
            {
                float maxOcclusion = 0.0f;
                for (int s = 0; s < segCount; ++s)
                {
                    ShadowSegment2DData seg = u_ShadowSegments[s];
                    int casterId = int(round(seg.Params.y));
                    bool selfShadows = (seg.Params.z > 0.5f);
                    if (input.v_EntityID >= 0 && casterId == input.v_EntityID && !selfShadows)
                        continue;

                    if (SegmentsIntersect2D(fragPos2D, lightPos, seg.Endpoints.xy, seg.Endpoints.zw))
                        maxOcclusion = max(maxOcclusion, saturate(seg.Params.x));
                }
                shadowVisibility = 1.0f - maxOcclusion;
            }
            if (shadowVisibility <= 0.0001f)
                continue;

            float diffuse = 1.0f;
            if (hasNormalMap)
            {
                float normalZ = max(light.Params.y, 0.05f);
                float3 lightVec3D = normalize(float3(toLight, normalZ));
                diffuse = max(dot(N, lightVec3D), 0.0f);
            }

            totalLight += light.ColorAndIntensity.rgb * (light.ColorAndIntensity.w * radialAtten * spotAtten * shadowVisibility * diffuse);
        }

        finalColor.rgb *= totalLight;
    }

    output.Color = finalColor;
    // 传递 ID (通常用于鼠标点击选中物体)
    output.EntityID = input.v_EntityID;

    return output;
}
