// ============================================================================
// WorldEngine Sky Dome & Celestial Atmosphere Shader (SkyDome.slang)
// High-Fidelity Celestial Atmosphere, Physically-Proportioned Sun Disc & Focused Corona,
// Dual-Layer Multi-Octave Fluffy Cumulus & Cirrus Clouds with Silver-Lining & Optical Scattering.
// ============================================================================

struct VS_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float3 a_Normal : NORMAL;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 v_ScreenPos : SCREENPOS;
};

struct PS_OUTPUT
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

// Set 0, Binding 0: 全局相机 UBO
[[vk::binding(0, 0)]] cbuffer CameraUniforms
{
    float4x4 u_ViewProjection;
};

// Set 1, Binding 0: 天空球与大气全要素参数 UBO (std140 对齐)
[[vk::binding(0, 1)]] cbuffer SkyUniforms
{
    float4x4 u_InvViewProjection;    // 逆视图投影矩阵
    float4 u_SkyZenithColor;         // rgb = 天顶深空色, a = 1.0
    float4 u_SkyHorizonColor;        // rgb = 地平线色, a = 1.0
    float4 u_SkyGroundColor;         // rgb = 地面基底色, a = 1.0
    float4 u_SunDirectionIntensity;  // xyz = 太阳光传导方向, w = 太阳光照强度
    float4 u_SunColorDisc;           // rgb = 太阳光照颜色, a = 太阳视盘大小
    float4 u_MoonDirectionIntensity; // xyz = 月球光传导方向, w = 月球光照强度
    float4 u_MoonColorDisc;          // rgb = 月球光照颜色, a = 月球视盘大小
    float4 u_CloudParams;            // x = 云层覆盖率, y = 云层密度, z = 云层缩放, w = 软化度
    float4 u_CloudColor;             // rgb = 云层固有色, a = 阴云暗化因子
    float4 u_WindDriftTime;          // xy = 风场漂移偏移, z = 场景运行时间, w = 星空可见度
    float4 u_CameraPosition;         // xyz = 相机世界空间坐标, w = 曝光系数
};

// ----------------------------------------------------------------------------
// 顶点阶段: 屏幕空间四边形
// ----------------------------------------------------------------------------
[shader("vertex")]
VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    output.Position = float4(input.a_Position.xy, 1.0f, 1.0f);
    output.v_ScreenPos = input.a_Position.xy;
    return output;
}

// ----------------------------------------------------------------------------
// 优质高频哈希与 C2 连续五次多项式样条插值
// ----------------------------------------------------------------------------
float Hash21(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float ValueNoise2D(float2 uv)
{
    float2 i = floor(uv);
    float2 f = frac(uv);
    
    // Ken Perlin 五次连续平滑 S 曲线: 6f^5 - 15f^4 + 10f^3 (一阶与二阶导数恒连续)
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    
    float a = Hash21(i + float2(0.0f, 0.0f));
    float b = Hash21(i + float2(1.0f, 0.0f));
    float c = Hash21(i + float2(0.0f, 1.0f));
    float d = Hash21(i + float2(1.0f, 1.0f));
    
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// 工业级积云分形噪声 (带多八度旋转与花椰菜团块侵蚀)
float CloudFractalNoise(float2 uv)
{
    float val = 0.0f;
    // Octave 1: 宏观大团块
    val += ValueNoise2D(uv) * 0.50f;
    
    // Octave 2: 正交旋转中尺度湍流
    float2 m = float2(0.80f * uv.x - 0.60f * uv.y, 0.60f * uv.x + 0.80f * uv.y) * 2.03f + float2(1.7f, 9.2f);
    val += ValueNoise2D(m) * 0.28f;
    
    // Octave 3: 侵蚀卷须细节
    float2 m2 = float2(0.80f * m.x - 0.60f * m.y, 0.60f * m.x + 0.80f * m.y) * 2.11f + float2(8.3f, 2.8f);
    val += ValueNoise2D(m2) * 0.15f;
    
    // Octave 4: 高频羽化毛边
    float2 m3 = float2(0.80f * m2.x - 0.60f * m2.y, 0.60f * m2.x + 0.80f * m2.y) * 2.35f + float2(3.5f, 5.1f);
    val += ValueNoise2D(m3) * 0.07f;
    
    return val;
}

// 高空半透明丝状卷云噪声 (Cirrus Wisps)
float CirrusFractalNoise(float2 uv)
{
    float2 stretchUV = float2(uv.x * 2.5f + uv.y * 0.8f, uv.y * 0.6f);
    float n1 = ValueNoise2D(stretchUV * 1.5f);
    float n2 = ValueNoise2D(stretchUV * 3.2f + float2(5.1f, 2.3f));
    return n1 * 0.65f + n2 * 0.35f;
}

// ----------------------------------------------------------------------------
// 片段阶段: 逆投影精确视线、瑞利大气、紧致物理太阳天盘、多层高品质云海与繁星
// ----------------------------------------------------------------------------
[shader("fragment")]
PS_OUTPUT PSMain(VS_OUTPUT input)
{
    PS_OUTPUT output;
    
    // 1. 逆视图投影解算世界空间无穷远视线方向
    float4 clipPos = float4(input.v_ScreenPos.x, input.v_ScreenPos.y, 1.0f, 1.0f);
    float4 worldPoint = mul(u_InvViewProjection, clipPos);
    float3 worldPos = worldPoint.xyz / max(abs(worldPoint.w), 1e-6f);
    float3 dir = normalize(worldPos - u_CameraPosition.xyz);
    
    float elev = dir.y;
    
    // 2. 瑞利大气天光仰角平滑连续渐变 (Zenith -> Horizon -> Ground)
    float3 skyColor;
    if (elev >= 0.0f)
    {
        float t = pow(saturate(elev), 0.55f);
        skyColor = lerp(u_SkyHorizonColor.rgb, u_SkyZenithColor.rgb, t);
    }
    else
    {
        float t = pow(saturate(-elev * 4.0f), 0.50f);
        skyColor = lerp(u_SkyHorizonColor.rgb, u_SkyGroundColor.rgb, t);
    }
    
    // 太阳与月球光照传导方向
    float3 sunDir = normalize(u_SunDirectionIntensity.xyz);
    float3 moonDir = normalize(u_MoonDirectionIntensity.xyz);
    float sunIntensity = u_SunDirectionIntensity.w;
    float moonIntensity = u_MoonDirectionIntensity.w;
    
    // 3. 黄金时刻 / 晚霞与晨曦大气散射 (Twilight / Sunset Rayleigh Scatter)
    float sunDot = saturate(dot(dir, -sunDir));
    if (abs(sunDir.y) < 0.40f)
    {
        float sunsetFactor = pow(sunDot, 3.5f) * saturate(1.0f - abs(sunDir.y) * 2.8f);
        float3 sunsetColor = float3(1.0f, 0.45f, 0.15f);
        skyColor += sunsetColor * sunsetFactor * 1.5f;
    }
    
    // 4. 精致小巧物理比例太阳天盘与紧致日晕 (Focused Realistic Sun Disc & Mie Corona)
    // 解决“太阳过大、刺眼漫延半屏”的问题：严格控制在物理视直径约 0.65 度，日晕指数高频聚焦
    if (sunDir.y < 0.15f)
    {
        float cosSun = dot(dir, -sunDir);
        
        // 紧凑纯圆发光太阳天盘 (0.65° 视角角直径，小巧精致，中心纯白炽热)
        float sunRadius = 0.99986f;
        float sunDisc = smoothstep(sunRadius - 0.00008f, sunRadius + 0.00002f, cosSun);
        float3 sunCore = u_SunColorDisc.rgb * (3.8f + sunIntensity * 3.5f);
        
        // 紧致高品质日晕: 超高频金辉内晕 + 柔和次级散射，绝不大面积染白全屏
        float coronaInner = pow(saturate(cosSun), 2400.0f) * 1.5f;   // 紧紧围绕太阳边缘一圈金辉
        float coronaMid = pow(saturate(cosSun), 380.0f) * 0.22f;     // 柔和近距日晕
        float coronaOuter = pow(saturate(cosSun), 54.0f) * 0.04f;    // 弱大气泛光
        float3 corona = u_SunColorDisc.rgb * (coronaInner + coronaMid + coronaOuter) * (0.8f + sunIntensity * 1.5f);
        
        float sunFade = saturate((elev + 0.08f) * 8.0f) * saturate((-sunDir.y + 0.06f) * 8.0f);
        skyColor += (sunDisc * sunCore + corona) * sunFade;
    }
    
    // 5. 夜空月球天盘与柔和月华
    if (moonIntensity > 0.001f && moonDir.y < 0.15f)
    {
        float cosMoon = dot(dir, -moonDir);
        float moonRadius = 0.99990f;
        float moonDisc = smoothstep(moonRadius - 0.00006f, moonRadius + 0.00002f, cosMoon);
        float moonHalo = pow(saturate(cosMoon), 180.0f) * 0.40f;
        
        float moonFade = saturate((elev + 0.05f) * 8.0f) * saturate((-moonDir.y + 0.05f) * 8.0f);
        float3 moonColor = u_MoonColorDisc.rgb * (moonDisc * 2.2f + moonHalo * 0.8f) * moonFade;
        skyColor += moonColor;
    }
    
    // 6. 繁星夜空 (Twinkling Celestial Starfield)
    float starVisibility = u_WindDriftTime.w;
    if (starVisibility > 0.02f && elev > 0.04f)
    {
        float2 starCoord = float2(atan2(dir.z, dir.x) * 35.0f, elev * 70.0f);
        float2 starCell = floor(starCoord);
        float starHash = Hash21(starCell);
        
        if (starHash > 0.965f)
        {
            float2 starFrac = frac(starCoord) - 0.5f;
            float starDist = length(starFrac);
            float twinkle = 0.75f + 0.25f * sin(u_WindDriftTime.z * 4.0f + starHash * 30.0f);
            float starBrightness = saturate(1.0f - starDist * 6.0f) * twinkle;
            float starIntensity = starVisibility * starBrightness * (starHash - 0.965f) * 28.0f;
            skyColor += float3(0.92f, 0.95f, 1.0f) * starIntensity;
        }
    }
    
    // 7. 工业级多层高品质动态云海 (Dual-Layer Cirrus & Volumetric-Style Cumulus)
    float coverage = u_CloudParams.x;
    if (coverage > 0.02f && elev > 0.015f)
    {
        float horizonFade = saturate(elev * 4.5f);
        
        // (A) 高空半透明丝状卷云层 (Cirrus Wisps Layer at ~6000m)
        float2 cirrusUV = (dir.xz / (elev + 0.28f)) * (0.45f * u_CloudParams.z);
        cirrusUV += u_WindDriftTime.xy * 0.65f; // 高空气流平移
        float cirrusNoise = CirrusFractalNoise(cirrusUV);
        float cirrusAlpha = smoothstep(0.48f, 0.72f, cirrusNoise) * 0.35f * horizonFade;
        float3 cirrusColor = lerp(u_CloudColor.rgb * 0.85f, float3(1.0f, 1.0f, 1.0f), 0.5f);
        skyColor = lerp(skyColor, cirrusColor, cirrusAlpha);
        
        // (B) 低空中尺度蓬松积云海 (Main Cumulus Layer with Beer-Powder Optical Scattering)
        float2 cloudUV = (dir.xz / (elev + 0.16f)) * (0.75f * u_CloudParams.z);
        cloudUV += u_WindDriftTime.xy; // 动态主风场漂移
        
        float noise = CloudFractalNoise(cloudUV);
        
        // 蓬松云块阈值与平滑密度演变
        float cloudThreshold = 0.82f - coverage * 0.60f;
        float density = smoothstep(cloudThreshold - 0.16f, cloudThreshold + 0.20f, noise);
        float cloudAlpha = saturate(density * (0.88f + u_CloudParams.y * 0.50f));
        
        if (cloudAlpha > 0.005f)
        {
            // 物理向光银边前向散射 (Silver Lining / Forward Mie Scattering)
            float cosSunView = dot(dir, -sunDir);
            float forwardScatter = pow(saturate(cosSunView), 2.5f);
            float silverLining = pow(saturate(cosSunView), 6.0f) * 1.5f;
            
            // Beer-Powder 光学吸收与糖粉透射近似 (薄云透光亮白，厚云背光产生饱满自遮蔽阴影)
            float powderSugar = 1.0f - exp(-density * 2.5f);
            float opticalDepth = saturate(density * 1.2f);
            
            // 背光暗部吸收天光环境色 (柔和深邃天蓝灰)
            float3 cloudAmbient = u_CloudColor.rgb * float3(0.65f, 0.72f, 0.85f);
            // 向光高光金色亮缘
            float3 cloudHighlight = u_CloudColor.rgb * (1.20f + sunIntensity * 0.70f) + u_SunColorDisc.rgb * (silverLining * 0.5f);
            
            float lightFactor = saturate(forwardScatter * 0.65f + (1.0f - opticalDepth * 0.55f));
            float3 cloudLitColor = lerp(cloudAmbient, cloudHighlight, lightFactor * powderSugar);
            
            // 将主积云层柔和合成到天空中
            skyColor = lerp(skyColor, cloudLitColor, cloudAlpha * horizonFade * 0.94f);
        }
    }
    
    // 8. 颜色输出
    output.Color = float4(skyColor, 1.0f);
    output.EntityID = -1;
    return output;
}
