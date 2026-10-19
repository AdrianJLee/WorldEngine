#include "wldpch.h"
#include "World/Renderer/VfxModule.h"

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
			CreateGravityModule(),
			CreateLinearDragModule(),
			CreateCurlNoiseModule(),
			CreatePointAttractorModule(),
			CreateVortexModule(),
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
}
