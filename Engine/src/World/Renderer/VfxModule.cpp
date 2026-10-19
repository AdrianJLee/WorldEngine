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

	std::vector<VfxModule> VfxModuleLibrary::GetBuiltinModules()
	{
		return {
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
			CreateCustomSlangModule("自定义模拟代码块 (Custom Slang Block)")
		};
	}

	std::vector<VfxModule> VfxModuleLibrary::CreateDefaultEmitterModuleStack()
	{
		return {
			CreateConeSpawnModule(),
			CreateGravityModule(),
			CreateLinearDragModule(),
			CreateCurlNoiseModule(),
			CreateSolveVelocityModule()
		};
	}
}
