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
};

// 顶点输出 / 像素输入：数据在此结构体中进行插值
struct VS_OUTPUT
{
    // SV_Position 是内建语义，对应 GLSL 的 gl_Position
    float4 Position : SV_Position;
    
    [[vk::location(0)]] float4 v_Color : COLOR;
    [[vk::location(1)]] float2 v_TexCoord : TEXCOORD0;
    [[vk::location(2)]] float v_TexIndex : TEXCOORD1;
    [[vk::location(3)]] float v_TilingFactor : TILING;
    // nointerpolation 对应 GLSL 的 flat，防止整数被插值
    [[vk::location(4)]] nointerpolation int v_EntityID : ENTITY_ID;
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

    // 坐标变换 (注意 mul 顺序：矩阵在前为列主序运算)
    output.Position = mul(u_ViewProjection, float4(input.a_Position, 1.0f));

    return output;
}

// ==========================================
// 4. 像素着色器 (Pixel Shader)
// ==========================================

PS_OUTPUT PSMain(VS_OUTPUT input)
{
    PS_OUTPUT output;

    // 采样纹理：使用纹理对象的 Sample 方法
    // 增加 + 0.5f 解决插值过程中的浮点数精度丢失引发的向下截断问题
    float4 texColor = u_Textures[uint(input.v_TexIndex)].Sample(input.v_TexCoord * input.v_TilingFactor);
    
    // 最终颜色计算
    output.Color = texColor * input.v_Color;
    
    // 传递 ID (通常用于鼠标点击选中物体)
    output.EntityID = input.v_EntityID;

    return output;
}
