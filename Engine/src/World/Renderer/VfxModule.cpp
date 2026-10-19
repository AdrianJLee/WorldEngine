#include "wldpch.h"
#include "World/Renderer/VfxModule.h"
#include "World/Utils/Paths.h"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <filesystem>


namespace World
{
	VfxModuleParam* VfxModule::FindParam(const std::string& name)
	{
		for (auto& p : Parameters)
		{
			if (p.Name == name) return &p;
		}
		return nullptr;
	}

	const VfxModuleParam* VfxModule::FindParam(const std::string& name) const
	{
		for (const auto& p : Parameters)
		{
			if (p.Name == name) return &p;
		}
		return nullptr;
	}

	float VfxModule::GetFloat(const std::string& name, float def) const
	{
		const auto* p = FindParam(name);
		return p ? p->FloatVal : def;
	}

	glm::vec4 VfxModule::GetVec4(const std::string& name, const glm::vec4& def) const
	{
		const auto* p = FindParam(name);
		return p ? p->VecVal : def;
	}

	void VfxModule::SetFloat(const std::string& name, float v)
	{
		auto* p = FindParam(name);
		if (p) p->FloatVal = v;
	}

	void VfxModule::SetVec4(const std::string& name, const glm::vec4& v)
	{
		auto* p = FindParam(name);
		if (p) p->VecVal = v;
	}

	// =========================================================================
	// 内建标准 Slang 模块库实现 (All Native Slang Code Blocks)
	// =========================================================================

	VfxModule VfxModuleLibrary::CreateConeSpawnModule()
	{
		VfxModule m;
		m.Id = "mod.spawn.cone";
		m.Name = "圆锥发射 (Cone Spawn)";
		m.Description = "按圆锥顶角与底面半径空间分布发射粒子并赋予初始初速度";
		m.Stage = VfxModuleStage::Spawn;
		m.Enabled = true;

		m.Parameters = {
			{ "ConeAngle", VfxParamType::Float, 30.0f, {}, 0, false, 0.0f, 90.0f, 1.0f, "圆锥半角 (度)" },
			{ "ConeRadius", VfxParamType::Float, 0.5f, {}, 0, false, 0.0f, 10.0f, 0.05f, "底部圆盘半径 (米)" },
			{ "InitialSpeed", VfxParamType::Float, 5.0f, {}, 0, false, 0.0f, 50.0f, 0.2f, "发射初始基准速率 (m/s)" },
			{ "SpeedVariance", VfxParamType::Float, 1.0f, {}, 0, false, 0.0f, 20.0f, 0.1f, "发射速度随机扰动范围" }
		};

		m.SlangSource = R"(
    // [Module: ConeSpawn] 圆锥发射空间分布算法
    float angleRad = radians(u_ConeAngle);
    float u = Hash1D(particleId * 3 + 1);
    float v = Hash1D(particleId * 3 + 2);
    float phi = 6.2831853f * u;
    float sinTheta = sin(angleRad) * sqrt(v);
    float cosTheta = sqrt(max(0.0f, 1.0f - sinTheta * sinTheta));
    float3 localDir = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
    
    float diskR = u_ConeRadius * sqrt(Hash1D(particleId * 3 + 0));
    float diskPhi = 6.2831853f * Hash1D(particleId * 3 + 7);
    p.Position = float3(diskR * cos(diskPhi), 0.0f, diskR * sin(diskPhi));
    
    float speed = u_InitialSpeed + (Hash1D(particleId * 7 + 3) * 2.0f - 1.0f) * u_SpeedVariance;
    p.Velocity = localDir * speed;
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateSphereSpawnModule()
	{
		VfxModule m;
		m.Id = "mod.spawn.sphere";
		m.Name = "球体发射 (Sphere Spawn)";
		m.Description = "在 3D 球体表面或体积内均匀随机生成粒子并向外辐射扩散";
		m.Stage = VfxModuleStage::Spawn;
		m.Enabled = true;

		m.Parameters = {
			{ "SphereRadius", VfxParamType::Float, 1.0f, {}, 0, false, 0.05f, 20.0f, 0.1f, "球体生成半径" },
			{ "OutwardSpeed", VfxParamType::Float, 4.0f, {}, 0, false, 0.0f, 50.0f, 0.2f, "沿径向向外喷射速度" }
		};

		m.SlangSource = R"(
    // [Module: SphereSpawn] 球面随机分布
    float z = 1.0f - 2.0f * Hash1D(particleId * 4 + 1);
    float r = sqrt(max(0.0f, 1.0f - z * z));
    float phi = 6.2831853f * Hash1D(particleId * 4 + 2);
    float3 normalDir = float3(r * cos(phi), z, r * sin(phi));
    p.Position = normalDir * u_SphereRadius;
    p.Velocity = normalDir * u_OutwardSpeed;
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateBoxSpawnModule()
	{
		VfxModule m;
		m.Id = "mod.spawn.box";
		m.Name = "立方盒发射 (Box Spawn)";
		m.Description = "在三维包围盒体积内随机生成粒子分布";
		m.Stage = VfxModuleStage::Spawn;
		m.Enabled = true;

		m.Parameters = {
			{ "BoxExtents", VfxParamType::Vec3, 0.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f), 0, false, 0.05f, 20.0f, 0.1f, "包围盒半尺寸 [X, Y, Z]" },
			{ "Speed", VfxParamType::Float, 2.0f, {}, 0, false, 0.0f, 30.0f, 0.1f, "向上漂移速度" }
		};

		m.SlangSource = R"(
    // [Module: BoxSpawn]
    p.Position = float3(
        (Hash1D(particleId * 5 + 0) * 2.0f - 1.0f) * u_BoxExtents.x,
        (Hash1D(particleId * 5 + 1) * 2.0f - 1.0f) * u_BoxExtents.y,
        (Hash1D(particleId * 5 + 2) * 2.0f - 1.0f) * u_BoxExtents.z
    );
    p.Velocity = float3(0.0f, u_Speed, 0.0f);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreatePointSpawnModule()
	{
		VfxModule m;
		m.Id = "mod.spawn.point";
		m.Name = "单点发射 (Point Spawn)";
		m.Description = "从单一原点向指定方向发射粒子";
		m.Stage = VfxModuleStage::Spawn;
		m.Enabled = true;

		m.Parameters = {
			{ "Speed", VfxParamType::Float, 5.0f, {}, 0, false, 0.0f, 50.0f, 0.2f, "发射速度" }
		};

		m.SlangSource = R"(
    // [Module: PointSpawn]
    p.Position = float3(0.0f);
    p.Velocity = float3(0.0f, u_Speed, 0.0f);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateGravityModule()
	{
		VfxModule m;
		m.Id = "mod.update.gravity";
		m.Name = "重力场 (Gravity Force)";
		m.Description = "对粒子施加恒定三维重力加速度向量";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;

		m.Parameters = {
			{ "Gravity", VfxParamType::Vec3, 0.0f, glm::vec4(0.0f, -9.8f, 0.0f, 0.0f), 0, false, -50.0f, 50.0f, 0.2f, "三维重力向量 [Gx, Gy, Gz]" }
		};

		m.SlangSource = R"(
    // [Module: GravityForce]
    p.Velocity += u_Gravity.xyz * dt;
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateLinearDragModule()
	{
		VfxModule m;
		m.Id = "mod.update.drag";
		m.Name = "空气阻力 (Air Drag)";
		m.Description = "模拟流体介质空气阻尼，阻碍粒子高速运动";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;

		m.Parameters = {
			{ "LinearDrag", VfxParamType::Float, 0.5f, {}, 0, false, 0.0f, 10.0f, 0.05f, "空气阻尼系数" }
		};

		m.SlangSource = R"(
    // [Module: LinearDrag]
    p.Velocity *= max(0.0f, 1.0f - u_LinearDrag * dt);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateCurlNoiseModule()
	{
		VfxModule m;
		m.Id = "mod.update.curl_noise";
		m.Name = "3D 旋度噪声场 (Curl Noise Turbulence)";
		m.Description = "基于无散度 3D 旋度噪声向量场驱动真实的流体涡流混沌运动";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;

		m.Parameters = {
			{ "NoiseStrength", VfxParamType::Float, 3.0f, {}, 0, false, 0.0f, 30.0f, 0.2f, "噪声场作用力强度", true, "TurbulenceStrength" },
			{ "NoiseFrequency", VfxParamType::Float, 0.8f, {}, 0, false, 0.05f, 10.0f, 0.05f, "空间采样频率 (波长缩放)" }
		};

		m.SlangSource = R"(
    // [Module: CurlNoiseTurbulence] 3D 无散度流体涡流场
    float3 curl = CurlNoise3D(p.Position * u_NoiseFrequency);
    p.Velocity += curl * (u_NoiseStrength * dt);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreatePointAttractorModule()
	{
		VfxModule m;
		m.Id = "mod.update.attractor";
		m.Name = "点引力/黑洞吸引子 (Point Attractor)";
		m.Description = "向指定 3D 空间点施加平方反比万有引力吸引（或排斥）力场";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;

		m.Parameters = {
			{ "AttractorCenter", VfxParamType::Vec3, 0.0f, glm::vec4(0.0f, 1.0f, 0.0f, 0.0f), 0, false, -50.0f, 50.0f, 0.1f, "引力中心 3D 坐标", true, "AttractorPos" },
			{ "AttractorStrength", VfxParamType::Float, 12.0f, {}, 0, false, -50.0f, 100.0f, 0.5f, "引力强度 (正为吸引，负为排斥)", true, "AttractorMass" },
			{ "MinDistance", VfxParamType::Float, 0.2f, {}, 0, false, 0.01f, 5.0f, 0.05f, "奇点安全最小距离 (防除零除以过小值)" }
		};

		m.SlangSource = R"(
    // [Module: PointAttractor] 平方反比中心引力场
    float3 toCenter = u_AttractorCenter.xyz - p.Position;
    float distSq = max(dot(toCenter, toCenter), u_MinDistance * u_MinDistance);
    float3 pullDir = normalize(toCenter);
    p.Velocity += pullDir * (u_AttractorStrength / distSq) * dt;
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateVortexModule()
	{
		VfxModule m;
		m.Id = "mod.update.vortex";
		m.Name = "轴向旋转涡流 (Vortex Field)";
		m.Description = "围绕指定三维轴线驱动粒子做切向飞旋龙卷运动";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;

		m.Parameters = {
			{ "VortexAxis", VfxParamType::Vec3, 0.0f, glm::vec4(0.0f, 1.0f, 0.0f, 0.0f), 0, false, -1.0f, 1.0f, 0.05f, "涡流旋转轴向量" },
			{ "VortexStrength", VfxParamType::Float, 4.0f, {}, 0, false, -30.0f, 30.0f, 0.2f, "旋转切向角速度强度" }
		};

		m.SlangSource = R"(
    // [Module: VortexField] 轴向切向旋转
    float3 normAxis = normalize(u_VortexAxis.xyz);
    float3 tangential = cross(normAxis, p.Position);
    p.Velocity += tangential * (u_VortexStrength * dt);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateSolveVelocityModule()
	{
		VfxModule m;
		m.Id = "mod.update.solve_velocity";
		m.Name = "物理积分与生命周期 (Solve Velocity & Life)";
		m.Description = "欧拉物理速度积分更新位置，递减生命周期并计算归一化时间";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;

		m.Parameters = {
			{ "SpeedDamping", VfxParamType::Float, 0.0f, {}, 0, false, 0.0f, 5.0f, 0.05f, "物理速度阻尼" }
		};

		m.SlangSource = R"(
    // [Module: SolveVelocity] 物理欧拉积分与消亡
    p.Position += p.Velocity * dt;
    p.Life -= dt;
    if (u_SpeedDamping > 0.001f)
        p.Velocity *= max(0.0f, 1.0f - u_SpeedDamping * dt);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateCustomSlangModule(const std::string& customName)
	{
		VfxModule m;
		m.Id = "mod.custom." + std::to_string(rand() % 100000);
		m.Name = customName.empty() ? "自定义 Slang 动力学 (Custom Slang)" : customName;
		m.Description = "用户编写的原生 Slang 计算代码块，可任意编写数学公式、场与非线性动力学";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;
		m.IsCustom = true;

		m.Parameters = {
			{ "CustomGain", VfxParamType::Float, 2.0f, {}, 0, false, 0.0f, 20.0f, 0.1f, "自定义增益因子", true, "CustomGain" }
		};

		m.SlangSource = R"(
    // [Custom Slang Code Block]
    // 可用变量: p.Position, p.Velocity, p.Color, p.Life, p.MaxLife, p.Size, dt
    // 示例: 李萨如 3D 轨迹 / 周期性波动
    float wave = sin(p.Position.y * 3.0f + p.Life * 4.0f);
    p.Velocity.x += wave * (u_CustomGain * dt);
    p.Velocity.z += cos(p.Position.y * 3.0f + p.Life * 4.0f) * (u_CustomGain * dt);
)";
		return m;
	}

		// =========================================================================
	// 标准一等公民模块 (First-Class Niagara Modules)
	// =========================================================================

	VfxModule VfxModuleLibrary::CreateSpawnRateModule(float rate)
	{
		VfxModule m;
		m.Id = "mod.emitter.spawn_rate";
		m.Name = "持续发射 (Spawn Rate)";
		m.Description = "控制每秒持续发射的粒子数量 (火焰/浓烟/喷泉/火花流)";
		m.Stage = VfxModuleStage::Emitter;
		m.Enabled = true;
		m.Parameters = {
			{ "SpawnRate", VfxParamType::Float, rate, {}, 0, false, 0.0f, 20000.0f, 1.0f, "每秒持续发射粒子数 (/s)", true, "EmissionRate" }
		};
		m.SlangSource = R"(
    // [Module: SpawnRate]
    // 由系统发射调度器根据 SpawnRate 与 dt 累积并生成新图元
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateSpawnBurstModule(uint32_t count, float time)
	{
		VfxModule m;
		m.Id = "mod.emitter.spawn_burst";
		m.Name = "瞬态爆发/单体 (Spawn Burst)";
		m.Description = "在指定时间瞬间爆发生成固定数量图元 (Count=1 用于法阵/护盾/光环，零发射流开销)";
		m.Stage = VfxModuleStage::Emitter;
		m.Enabled = true;
		m.Parameters = {
			{ "SpawnCount", VfxParamType::Float, static_cast<float>(count), {}, 0, false, 1.0f, 10000.0f, 1.0f, "瞬时生成图元数量 (1 为单体法阵/护盾)", true, "BurstCount" },
			{ "SpawnTime", VfxParamType::Float, time, {}, 0, false, 0.0f, 60.0f, 0.05f, "爆发触发时间 (秒)" }
		};
		m.SlangSource = R"(
    // [Module: SpawnBurst]
    // 在指定时间瞬间生成指定数量的粒子或单体网格图元
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateInitParticleModule(float lifetime, float sizeStart, float sizeEnd)
	{
		VfxModule m;
		m.Id = "mod.spawn.init_particle";
		m.Name = "粒子初始化 (Initialize Particle)";
		m.Description = "设定图元诞生时的基准生命周期、初始尺寸与渐变颜色";
		m.Stage = VfxModuleStage::Spawn;
		m.Enabled = true;
		m.Parameters = {
			{ "Lifetime", VfxParamType::Float, lifetime, {}, 0, false, 0.05f, 60.0f, 0.05f, "粒子存活基准寿命 (秒)", true, "Lifetime" },
			{ "SizeStart", VfxParamType::Float, sizeStart, {}, 0, false, 0.01f, 20.0f, 0.05f, "诞生初始尺寸", true, "SizeStart" },
			{ "SizeEnd", VfxParamType::Float, sizeEnd, {}, 0, false, 0.01f, 20.0f, 0.05f, "消亡终末尺寸", true, "SizeEnd" },
			{ "ColorStart", VfxParamType::Vec4, 0.0f, glm::vec4(1.0f, 0.8f, 0.3f, 1.0f), 0, false, 0.0f, 1.0f, 0.05f, "初始 RGBA 颜色" },
			{ "ColorEnd", VfxParamType::Vec4, 0.0f, glm::vec4(1.0f, 0.2f, 0.0f, 0.0f), 0, false, 0.0f, 1.0f, 0.05f, "消亡 RGBA 颜色" }
		};
		m.SlangSource = R"(
    // [Module: InitializeParticle]
    p.MaxLife = u_Lifetime;
    p.Life = u_Lifetime;
    p.Size = float2(u_SizeStart);
    p.Color = u_ColorStart;
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateSpriteRendererModule(bool additive, bool horizontal, float emissive)
	{
		VfxModule m;
		m.Id = "mod.render.sprite";
		m.Name = "精灵渲染器 (Sprite Renderer)";
		m.Description = "配置图元混合模式 (加法/半透)、自发光强度与面片朝向 (相机对齐/水平地面)";
		m.Stage = VfxModuleStage::Render;
		m.Enabled = true;
		m.Parameters = {
			{ "BlendMode", VfxParamType::Float, additive ? 1.0f : 0.0f, {}, 0, false, 0.0f, 1.0f, 1.0f, "混合模式 (0=半透 AlphaBlend, 1=加法 Additive)" },
			{ "FacingMode", VfxParamType::Float, horizontal ? 1.0f : 0.0f, {}, 0, false, 0.0f, 1.0f, 1.0f, "面片朝向 (0=Billboard相机对齐, 1=Horizontal地面水平)" },
			{ "EmissiveBoost", VfxParamType::Float, emissive, {}, 0, false, 0.1f, 20.0f, 0.1f, "HDR 自发光倍率 (促发辉光 Bloom)", true, "EmissiveBoost" }
		};
		m.SlangSource = R"(
    // [Module: SpriteRenderer]
    // 驱动 GPU 混合模式与几何着色器面片顶点朝向生成
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateRingSpawnModule(float radius, float initialSpeed)
	{
		VfxModule m;
		m.Id = "mod.spawn.ring";
		m.Name = "圆环发射 (Ring Spawn)";
		m.Description = "在 3D 水平圆环边缘几何分布生成粒子 (法阵/护盾/光环/冰霜新星)";
		m.Stage = VfxModuleStage::Spawn;
		m.Enabled = true;
		m.Parameters = {
			{ "RingRadius", VfxParamType::Float, radius, {}, 0, false, 0.1f, 20.0f, 0.1f, "圆环生成半径 (m)" },
			{ "InitialSpeed", VfxParamType::Float, initialSpeed, {}, 0, false, 0.0f, 50.0f, 0.2f, "垂直发射速率 (m/s)" }
		};
		m.SlangSource = R"(
    // [Module: RingSpawn] 水平圆环边缘几何分布
    float u = Hash1D(particleId * 11u + 3u);
    float phi = 6.2831853f * u;
    p.Position = float3(cos(phi) * u_RingRadius, 0.0f, sin(phi) * u_RingRadius);
    p.Velocity = float3(0.0f, u_InitialSpeed, 0.0f);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateScaleOverLifeModule(float startScale, float endScale)
	{
		VfxModule m;
		m.Id = "mod.update.scale_over_life";
		m.Name = "尺寸随寿命缩放 (Scale Over Life)";
		m.Description = "基于归一化存活进度 (0~1) 控制粒子尺寸膨胀扩散或缩小消亡";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;
		m.Parameters = {
			{ "StartScale", VfxParamType::Float, startScale, {}, 0, false, 0.0f, 20.0f, 0.05f, "初始尺寸乘子" },
			{ "EndScale", VfxParamType::Float, endScale, {}, 0, false, 0.0f, 20.0f, 0.05f, "消亡前终止尺寸乘子" }
		};
		m.SlangSource = R"(
    // [Module: Scale Over Life]
    float normLife = clamp(1.0f - (p.Life / max(0.001f, p.MaxLife)), 0.0f, 1.0f);
    p.Size = lerp(u_StartScale, u_EndScale, normLife);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateColorOverLifeModule(const glm::vec4& colorStart, const glm::vec4& colorEnd)
	{
		VfxModule m;
		m.Id = "mod.update.color_over_life";
		m.Name = "色彩淡入淡出 (Color Over Life)";
		m.Description = "基于粒子存活进度进行色彩与透明度渐变过渡";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;
		m.Parameters = {
			{ "ColorStart", VfxParamType::Vec4, 1.0f, colorStart, 0, false, 0.0f, 1.0f, 0.01f, "初始出生颜色 (RGBA)" },
			{ "ColorEnd", VfxParamType::Vec4, 1.0f, colorEnd, 0, false, 0.0f, 1.0f, 0.01f, "终止消亡颜色 (RGBA)" }
		};
		m.SlangSource = R"(
    // [Module: Color Over Life]
    float normLife = clamp(1.0f - (p.Life / max(0.001f, p.MaxLife)), 0.0f, 1.0f);
    p.Color = lerp(u_ColorStart, u_ColorEnd, normLife);
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateOrbitModule(float speed, const glm::vec3& axis, const glm::vec3& center)
	{
		VfxModule m;
		m.Id = "mod.update.orbit";
		m.Name = "中心轴轨道公转 (Orbit Around Center)";
		m.Description = "围绕指定空间中心点与旋转轴进行稳定圆周轨道旋转运动 (护盾/光环/符文)";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;
		m.Parameters = {
			{ "OrbitSpeed", VfxParamType::Float, speed, {}, 0, false, -30.0f, 30.0f, 0.2f, "轨道公转切向角速率" },
			{ "OrbitAxis", VfxParamType::Vec3, 1.0f, glm::vec4(axis, 0.0f), 0, false, -1.0f, 1.0f, 0.1f, "公转旋转主轴" },
			{ "OrbitCenter", VfxParamType::Vec3, 1.0f, glm::vec4(center, 0.0f), 0, false, -50.0f, 50.0f, 0.5f, "空间轨道几何中心点" }
		};
		m.SlangSource = R"(
    // [Module: Orbit Around Center]
    float3 toCenter = p.Position - u_OrbitCenter.xyz;
    float dist = length(toCenter);
    if (dist > 0.01f)
    {
        float3 tangent = cross(u_OrbitAxis.xyz, normalize(toCenter));
        p.Velocity = tangent * u_OrbitSpeed;
    }
)";
		return m;
	}

	VfxModule VfxModuleLibrary::CreateJitterModule(float jitterStrength, float jitterSpeed)
	{
		VfxModule m;
		m.Id = "mod.update.jitter";
		m.Name = "高频抖动电弧 (Jitter / Electric Arc)";
		m.Description = "高频空间位移扰动，模拟雷电折线、电弧颤动与能量颤抖";
		m.Stage = VfxModuleStage::Update;
		m.Enabled = true;
		m.Parameters = {
			{ "JitterStrength", VfxParamType::Float, jitterStrength, {}, 0, false, 0.0f, 10.0f, 0.1f, "抖动偏移幅度" },
			{ "JitterSpeed", VfxParamType::Float, jitterSpeed, {}, 0, false, 1.0f, 100.0f, 1.0f, "抖动高频振荡速率" }
		};
		m.SlangSource = R"(
    // [Module: Jitter / Electric Arc]
    float3 jNoise = Noise3D(p.Position * 4.0f + float3(p.Life * u_JitterSpeed, 0, 0));
    p.Position += jNoise * (u_JitterStrength * dt);
)";
		return m;
	}

	std::vector<VfxModule> VfxModuleLibrary::GetBuiltinModules()
	{
		return {
			CreateSpawnRateModule(),
			CreateSpawnBurstModule(),
			CreateInitParticleModule(),
			CreateConeSpawnModule(),
			CreateSphereSpawnModule(),
			CreateBoxSpawnModule(),
			CreatePointSpawnModule(),
			CreateRingSpawnModule(),
			CreateGravityModule(),
			CreateLinearDragModule(),
			CreateCurlNoiseModule(),
			CreatePointAttractorModule(),
			CreateVortexModule(),
			CreateOrbitModule(),
			CreateScaleOverLifeModule(),
			CreateColorOverLifeModule(),
			CreateJitterModule(),
			CreateSolveVelocityModule(),
			CreateSpriteRendererModule(),
			CreateCustomSlangModule("自定义模拟代码块 (Custom Slang Block)")
		};
	}

	std::vector<VfxModule> VfxModuleLibrary::CreateDefaultEmitterModuleStack()
	{
		return {
			CreateSpawnRateModule(50.0f),
			CreateInitParticleModule(2.0f, 0.4f, 0.05f),
			CreateConeSpawnModule(),
			CreateGravityModule(),
			CreateLinearDragModule(),
			CreateCurlNoiseModule(),
			CreateSolveVelocityModule(),
			CreateSpriteRendererModule(true, false, 2.0f)
		};
	}

	// =========================================================================
	// 模块序列化与持久化实现 (YAML Serialization & Deserialization)
	// =========================================================================

	std::string VfxModule::SerializeToYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Id" << YAML::Value << Id;
		out << YAML::Key << "Name" << YAML::Value << Name;
		out << YAML::Key << "Description" << YAML::Value << Description;
		out << YAML::Key << "Stage" << YAML::Value << (Stage == VfxModuleStage::Emitter ? "Emitter" : (Stage == VfxModuleStage::Spawn ? "Spawn" : (Stage == VfxModuleStage::Update ? "Update" : "Render")));
		out << YAML::Key << "Enabled" << YAML::Value << Enabled;
		out << YAML::Key << "IsCustom" << YAML::Value << IsCustom;
		out << YAML::Key << "SlangSource" << YAML::Value << SlangSource;

		if (!Parameters.empty())
		{
			out << YAML::Key << "Parameters" << YAML::Value << YAML::BeginSeq;
			for (const auto& p : Parameters)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "Name" << YAML::Value << p.Name;
				out << YAML::Key << "Type" << YAML::Value << static_cast<int>(p.Type);
				out << YAML::Key << "FloatVal" << YAML::Value << p.FloatVal;
				out << YAML::Key << "VecVal" << YAML::Value << YAML::Flow << YAML::BeginSeq << p.VecVal.x << p.VecVal.y << p.VecVal.z << p.VecVal.w << YAML::EndSeq;
				out << YAML::Key << "IntVal" << YAML::Value << p.IntVal;
				out << YAML::Key << "BoolVal" << YAML::Value << p.BoolVal;
				out << YAML::Key << "Min" << YAML::Value << p.Min;
				out << YAML::Key << "Max" << YAML::Value << p.Max;
				out << YAML::Key << "Step" << YAML::Value << p.Step;
				out << YAML::Key << "Tooltip" << YAML::Value << p.Tooltip;
				out << YAML::Key << "IsExposed" << YAML::Value << p.IsExposed;
				out << YAML::Key << "ExposedName" << YAML::Value << p.ExposedName;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}
		out << YAML::EndMap;
		return out.c_str();
	}

	bool VfxModule::DeserializeFromYaml(const std::string& yamlStr, VfxModule& outMod)
	{
		try
		{
			YAML::Node mNode = YAML::Load(yamlStr);
			if (!mNode.IsMap()) return false;
			if (mNode["Id"]) outMod.Id = mNode["Id"].as<std::string>();
			if (mNode["Name"]) outMod.Name = mNode["Name"].as<std::string>();
			if (mNode["Description"]) outMod.Description = mNode["Description"].as<std::string>();
			if (mNode["Stage"])
			{
				std::string st = mNode["Stage"].as<std::string>();
				if (st == "Emitter") outMod.Stage = VfxModuleStage::Emitter;
				else if (st == "Spawn") outMod.Stage = VfxModuleStage::Spawn;
				else if (st == "Render") outMod.Stage = VfxModuleStage::Render;
				else outMod.Stage = VfxModuleStage::Update;
			}
			if (mNode["Enabled"]) outMod.Enabled = mNode["Enabled"].as<bool>();
			if (mNode["IsCustom"]) outMod.IsCustom = mNode["IsCustom"].as<bool>();
			if (mNode["SlangSource"]) outMod.SlangSource = mNode["SlangSource"].as<std::string>();

			outMod.Parameters.clear();
			if (const auto pSeq = mNode["Parameters"])
			{
				if (pSeq.IsSequence())
				{
					for (size_t j = 0; j < pSeq.size(); ++j)
					{
						const auto pNode = pSeq[j];
						VfxModuleParam p;
						if (pNode["Name"]) p.Name = pNode["Name"].as<std::string>();
						if (pNode["Type"]) p.Type = static_cast<VfxParamType>(pNode["Type"].as<int>());
						if (pNode["FloatVal"]) p.FloatVal = pNode["FloatVal"].as<float>();
						if (pNode["VecVal"] && pNode["VecVal"].IsSequence() && pNode["VecVal"].size() >= 4)
						{
							p.VecVal = glm::vec4(
								pNode["VecVal"][0].as<float>(),
								pNode["VecVal"][1].as<float>(),
								pNode["VecVal"][2].as<float>(),
								pNode["VecVal"][3].as<float>()
							);
						}
						if (pNode["IntVal"]) p.IntVal = pNode["IntVal"].as<int>();
						if (pNode["BoolVal"]) p.BoolVal = pNode["BoolVal"].as<bool>();
						if (pNode["Min"]) p.Min = pNode["Min"].as<float>();
						if (pNode["Max"]) p.Max = pNode["Max"].as<float>();
						if (pNode["Step"]) p.Step = pNode["Step"].as<float>();
						if (pNode["Tooltip"]) p.Tooltip = pNode["Tooltip"].as<std::string>();
						if (pNode["IsExposed"]) p.IsExposed = pNode["IsExposed"].as<bool>();
						if (pNode["ExposedName"]) p.ExposedName = pNode["ExposedName"].as<std::string>();
						outMod.Parameters.push_back(p);
					}
				}
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	// =========================================================================
	// 项目全局自定义模块持久化与库管理实现 (Project-Global Custom Slang Modules)
	// =========================================================================

	std::filesystem::path VfxModuleLibrary::GetProjectModulesDirectory()
	{
		return Paths::AssetRoot() / "vfx" / "modules";
	}

	std::vector<VfxModule> VfxModuleLibrary::LoadProjectCustomModules()
	{
		std::vector<VfxModule> result;
		std::error_code ec;

		const auto scanDir = [&](const std::filesystem::path& dir) {
			if (!std::filesystem::exists(dir, ec)) return;
			for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
			{
				if (entry.is_regular_file(ec) && entry.path().extension() == ".wvfxmod")
				{
					std::ifstream in(entry.path());
					if (in.is_open())
					{
						std::stringstream ss;
						ss << in.rdbuf();
						VfxModule mod;
						if (VfxModule::DeserializeFromYaml(ss.str(), mod))
						{
							mod.IsCustom = true;
							if (mod.Id.empty())
							{
								mod.Id = "mod.custom." + entry.path().stem().string();
							}
							bool exists = false;
							for (const auto& existing : result)
							{
								if (existing.Id == mod.Id || existing.Name == mod.Name)
								{
									exists = true;
									break;
								}
							}
							if (!exists)
							{
								result.push_back(mod);
							}
						}
					}
				}
			}
		};

		// 优先扫描当前项目资产目录: assets/vfx/modules/
		scanDir(GetProjectModulesDirectory());

		// 其次扫描引擎内容库目录: Engine/content/vfx/modules/
		scanDir(Paths::EngineContentRoot() / "vfx" / "modules");

		return result;
	}

	bool VfxModuleLibrary::SaveProjectCustomModule(const VfxModule& mod)
	{
		std::error_code ec;
		auto dir = GetProjectModulesDirectory();
		std::filesystem::create_directories(dir, ec);

		std::string safeName;
		for (char c : mod.Name)
		{
			if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
				safeName += '_';
			else
				safeName += c;
		}
		if (safeName.empty())
		{
			safeName = mod.Id.empty() ? "CustomModule" : mod.Id;
		}

		auto filepath = dir / (safeName + ".wvfxmod");
		std::ofstream out(filepath);
		if (!out.is_open()) return false;

		out << mod.SerializeToYaml();
		return true;
	}

	bool VfxModuleLibrary::DeleteProjectCustomModule(const std::string& moduleIdOrName)
	{
		std::error_code ec;
		auto dir = GetProjectModulesDirectory();
		if (!std::filesystem::exists(dir, ec)) return false;

		for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
		{
			if (entry.is_regular_file(ec) && entry.path().extension() == ".wvfxmod")
			{
				if (entry.path().stem().string() == moduleIdOrName)
				{
					return std::filesystem::remove(entry.path(), ec);
				}

				std::ifstream in(entry.path());
				if (in.is_open())
				{
					std::stringstream ss;
					ss << in.rdbuf();
					in.close();
					VfxModule mod;
					if (VfxModule::DeserializeFromYaml(ss.str(), mod))
					{
						if (mod.Id == moduleIdOrName || mod.Name == moduleIdOrName)
						{
							return std::filesystem::remove(entry.path(), ec);
						}
					}
				}
			}
		}
		return false;
	}
}
