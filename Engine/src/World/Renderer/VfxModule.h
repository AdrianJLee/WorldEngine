#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace World
{
	enum class VfxModuleStage : uint8_t
	{
		Emitter = 0, // 发射控制阶段 (Spawn Rate / Spawn Burst)
		Spawn = 1,   // 粒子出生初始化阶段 (Initialize Particle / Shape Distribution)
		Update = 2,  // 粒子动力学受力与更新阶段 (Forces / Drag / Turbulence / Solve)
		Render = 3   // 渲染器阶段 (Sprite / Mesh Renderer / BlendMode)
	};

	enum class VfxParamType : uint8_t
	{
		Float = 0,
		Vec2 = 1,
		Vec3 = 2,
		Vec4 = 3,
		Bool = 4,
		Int = 5
	};

	struct WLD_API VfxModuleParam
	{
		std::string Name;
		VfxParamType Type = VfxParamType::Float;
		float FloatVal = 0.0f;
		glm::vec4 VecVal { 0.0f };
		int IntVal = 0;
		bool BoolVal = false;
		float Min = 0.0f;
		float Max = 100.0f;
		float Step = 0.1f;
		std::string Tooltip;
		
		// 外部与运行时参数化支持
		bool IsExposed = false;          // 是否暴露为系统级参数 (外部/Inspector/Lua 可调)
		std::string ExposedName;         // 暴露名 (为空时默认采用 Name)

		std::string GetUniformName() const
		{
			return "u_" + (IsExposed && !ExposedName.empty() ? ExposedName : Name);
		}
	};

	struct WLD_API VfxModule
	{
		std::string Id;
		std::string Name;
		std::string Description;
		VfxModuleStage Stage = VfxModuleStage::Update;
		bool Enabled = true;
		bool IsCustom = false;          // 用户手写自定义 Slang 代码块
		std::vector<VfxModuleParam> Parameters;
		std::string SlangSource;        // 核心 Slang 代码块源码

		VfxModuleParam* FindParam(const std::string& name);
		const VfxModuleParam* FindParam(const std::string& name) const;
		float GetFloat(const std::string& name, float def = 0.0f) const;
		glm::vec4 GetVec4(const std::string& name, const glm::vec4& def = glm::vec4(0.0f)) const;
		void SetFloat(const std::string& name, float v);
		void SetVec4(const std::string& name, const glm::vec4& v);
	};

	class WLD_API VfxModuleLibrary
	{
	public:
		// 获取全部内建标准模块清单 (全部基于原生 Slang 代码实现)
		static std::vector<VfxModule> GetBuiltinModules();

		// Emitter 阶段标准模块 (发射与生成控制)
		static VfxModule CreateSpawnRateModule(float rate = 50.0f);
		static VfxModule CreateSpawnBurstModule(uint32_t count = 1, float time = 0.0f);

		// Particle Spawn 阶段标准模块 (初始化与几何分布)
		static VfxModule CreateInitParticleModule(float lifetime = 2.0f, float sizeStart = 0.3f, float sizeEnd = 0.05f);
		static VfxModule CreateConeSpawnModule();
		static VfxModule CreateSphereSpawnModule();
		static VfxModule CreateBoxSpawnModule();
		static VfxModule CreatePointSpawnModule();

		// Update 阶段标准模块
		static VfxModule CreateGravityModule();
		static VfxModule CreateLinearDragModule();
		static VfxModule CreateCurlNoiseModule();
		static VfxModule CreatePointAttractorModule();
		static VfxModule CreateVortexModule();
		static VfxModule CreateSolveVelocityModule();

		// Render 阶段标准模块 (渲染器与材质设定)
		static VfxModule CreateSpriteRendererModule(bool additive = true, bool horizontal = false, float emissive = 2.0f);

		// 自定义 Slang 代码块工厂
		static VfxModule CreateCustomSlangModule(const std::string& customName = "CustomSimulationBlock");

		// 默认工业级标准发射器模块堆栈 (Cone + Gravity + Drag + Noise + Solve)
		static std::vector<VfxModule> CreateDefaultEmitterModuleStack();
	};
}
