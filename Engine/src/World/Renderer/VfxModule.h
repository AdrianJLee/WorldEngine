#pragma once

#include "World/Core/Export.h"
#include "World/Math/Curve/CubicBezierCurve.h"
#include <glm/glm.hpp>
#include <string>
#include <filesystem>
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

	enum class VfxParamMode : uint8_t
	{
		Constant = 0,    // 常量模式
		RandomRange = 1, // 随机区间模式
		Curve = 2        // 随时间/寿命变化曲线模式
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

		// 动态求值模式与内嵌曲线 (置于末尾保证 C++17 聚合初始化后向兼容)
		VfxParamMode Mode = VfxParamMode::Constant;
		float RandomMin = 0.0f;
		float RandomMax = 1.0f;
		CubicBezierCurve Curve;       // 随时间/寿命变化的专属内嵌曲线
		bool CurveExpanded = true;    // 是否展开内嵌曲线编辑器
		glm::vec4 RandomMinVec { 0.0f }; // 向量/颜色随机区间下限 (Min)
		glm::vec4 RandomMaxVec { 1.0f }; // 向量/颜色随机区间上限 (Max)

		// 整体控制 vs 拆解分轴/通道支持 (对齐虚幻引擎 Niagara 标准: 向量拆成3个数，颜色拆成4个数)
		bool SplitAxes = false;          // 向量是否拆解为独立分量 (X / Y / Z 三通道数)
		VfxParamMode AxisMode[3] { VfxParamMode::Constant, VfxParamMode::Constant, VfxParamMode::Constant };
		float AxisRandomMin[3] { -10.0f, -10.0f, -10.0f };
		float AxisRandomMax[3] { 10.0f, 10.0f, 10.0f };
		CubicBezierCurve AxisCurve[3];   // X / Y / Z 三通道独立贝塞尔曲线
		bool AxisCurveExpanded[3] { false, false, false };
		int ActiveAxisCurveChannel = 0;  // 当前激活选中的曲线编辑通道 (0:X, 1:Y, 2:Z, -1:全部显示)

		bool SplitColor = false;         // 颜色是否拆解为独立分量 (R / G / B / A 四通道数)
		VfxParamMode ColorMode[4] { VfxParamMode::Constant, VfxParamMode::Constant, VfxParamMode::Constant, VfxParamMode::Constant };
		float ColorRandomMin[4] { 0.0f, 0.0f, 0.0f, 0.0f };
		float ColorRandomMax[4] { 1.0f, 1.0f, 1.0f, 1.0f };
		CubicBezierCurve ColorCurves[4]; // R / G / B / A 四通道独立贝塞尔曲线
		bool ColorCurveExpanded[4] { false, false, false, false };
		int ActiveColorCurveChannel = 0; // 当前激活选中的曲线编辑通道 (0:R, 1:G, 2:B, 3:A, -1:全部显示)

		// 历史兼容字段 (AlphaCurve 与 AlphaMode 自动桥接到 ColorCurves[3])
		VfxParamMode AlphaMode = VfxParamMode::Curve;
		float AlphaVal = 1.0f;
		float AlphaRandomMin = 0.0f;
		float AlphaRandomMax = 1.0f;
		CubicBezierCurve AlphaCurve;
		bool AlphaCurveExpanded = false;

		std::string GetUniformName() const
		{
			return "u_" + (IsExposed && !ExposedName.empty() ? ExposedName : Name);
		}

		void EnsureCurve()
		{
			if (Curve.KeyframeCount() == 0)
			{
				Curve = CubicBezierCurve::EaseOut();
				Curve.PrecomputeLut(256);
			}
		}

		void EnsureColorCurves()
		{
			if (ColorCurves[0].KeyframeCount() == 0)
			{
				float r = (VecVal.x > 0.0f || VecVal.y > 0.0f || VecVal.z > 0.0f) ? VecVal.x : 1.0f;
				ColorCurves[0] = CubicBezierCurve::Linear();
				if (ColorCurves[0].KeyframeCount() >= 2)
				{
					ColorCurves[0].GetKeyframeMut(0).Value = r;
					ColorCurves[0].GetKeyframeMut(1).Value = r;
				}
				ColorCurves[0].PrecomputeLut(256);
			}
			if (ColorCurves[1].KeyframeCount() == 0)
			{
				float g = (VecVal.x > 0.0f || VecVal.y > 0.0f || VecVal.z > 0.0f) ? VecVal.y : 0.8f;
				ColorCurves[1] = CubicBezierCurve::Linear();
				if (ColorCurves[1].KeyframeCount() >= 2)
				{
					ColorCurves[1].GetKeyframeMut(0).Value = g;
					ColorCurves[1].GetKeyframeMut(1).Value = g * 0.4f;
				}
				ColorCurves[1].PrecomputeLut(256);
			}
			if (ColorCurves[2].KeyframeCount() == 0)
			{
				float b = (VecVal.x > 0.0f || VecVal.y > 0.0f || VecVal.z > 0.0f) ? VecVal.z : 0.2f;
				ColorCurves[2] = CubicBezierCurve::Linear();
				if (ColorCurves[2].KeyframeCount() >= 2)
				{
					ColorCurves[2].GetKeyframeMut(0).Value = b;
					ColorCurves[2].GetKeyframeMut(1).Value = 0.0f;
				}
				ColorCurves[2].PrecomputeLut(256);
			}
			if (ColorCurves[3].KeyframeCount() == 0)
			{
				if (AlphaCurve.KeyframeCount() > 0)
				{
					ColorCurves[3] = AlphaCurve;
				}
				else
				{
					ColorCurves[3] = CubicBezierCurve::EaseInOut();
				}
				ColorCurves[3].PrecomputeLut(256);
			}
		}

		void EnsureAlphaCurve()
		{
			EnsureColorCurves();
			AlphaCurve = ColorCurves[3];
		}

		void EnsureAxisCurves()
		{
			for (int a = 0; a < 3; ++a)
			{
				EnsureAxisCurve(a);
			}
		}

		void EnsureAxisCurve(int a)
		{
			if (a >= 0 && a < 3 && AxisCurve[a].KeyframeCount() == 0)
			{
				float v = (&VecVal.x)[a];
				AxisCurve[a] = CubicBezierCurve::Linear();
				if (AxisCurve[a].KeyframeCount() >= 2)
				{
					AxisCurve[a].GetKeyframeMut(0).Value = v;
					AxisCurve[a].GetKeyframeMut(1).Value = v;
				}
				AxisCurve[a].PrecomputeLut(256);
			}
		}

		glm::vec3 EvaluateVec3(float normalizedTime) const
		{
			glm::vec3 res(VecVal);
			if (SplitAxes)
			{
				for (int a = 0; a < 3; ++a)
				{
					if (AxisMode[a] == VfxParamMode::Curve)
						(&res.x)[a] = AxisCurve[a].EvaluateFastLut(normalizedTime);
				}
			}
			else if (Mode == VfxParamMode::Curve)
			{
				res.x = AxisCurve[0].EvaluateFastLut(normalizedTime);
				res.y = AxisCurve[1].EvaluateFastLut(normalizedTime);
				res.z = AxisCurve[2].EvaluateFastLut(normalizedTime);
			}
			return res;
		}

		glm::vec4 EvaluateColor(float normalizedTime) const
		{
			glm::vec4 res = VecVal;
			if (SplitColor)
			{
				for (int c = 0; c < 4; ++c)
				{
					if (ColorMode[c] == VfxParamMode::Curve)
						(&res.x)[c] = ColorCurves[c].EvaluateFastLut(normalizedTime);
				}
			}
			else if (Mode == VfxParamMode::Curve)
			{
				res.r = ColorCurves[0].EvaluateFastLut(normalizedTime);
				res.g = ColorCurves[1].EvaluateFastLut(normalizedTime);
				res.b = ColorCurves[2].EvaluateFastLut(normalizedTime);
				res.a = ColorCurves[3].EvaluateFastLut(normalizedTime);
			}
			return res;
		}


		float Evaluate(float normalizedTime) const
		{
			if (Mode == VfxParamMode::Curve)
			{
				return Curve.Evaluate(normalizedTime);
			}
			return FloatVal;
		}
	};

	struct WLD_API VfxModule
	{
		std::string Id;
		std::string Name;
		std::string Description;
		VfxModuleStage Stage = VfxModuleStage::Update;
		bool Enabled = true;
		bool IsCustom = false;
		bool ShowGizmo = true; // 在视口中显示此几何形态范围框线框 (对齐 UE Niagara Show Gizmo)          // 用户手写自定义 Slang 代码块
		std::vector<VfxModuleParam> Parameters;
		std::string SlangSource;        // 核心 Slang 代码块源码

		VfxModuleParam* FindParam(const std::string& name);
		const VfxModuleParam* FindParam(const std::string& name) const;
		float GetFloat(const std::string& name, float def = 0.0f) const;
		glm::vec4 GetVec4(const std::string& name, const glm::vec4& def = glm::vec4(0.0f)) const;
		void SetFloat(const std::string& name, float v);
		void SetVec4(const std::string& name, const glm::vec4& v);

		std::string SerializeToYaml() const;
		static bool DeserializeFromYaml(const std::string& yamlStr, VfxModule& outMod);
	};

	class WLD_API VfxModuleLibrary
	{
	public:
		// 获取全部内建标准模块清单 (全部基于原生 Slang 代码实现)
		static std::vector<VfxModule> GetBuiltinModules();

		// Emitter 阶段标准模块 (发射与生成控制)
		static VfxModule CreateSpawnRateModule(float rate = 50.0f, uint32_t maxParticles = 500);
		static VfxModule CreateSpawnBurstModule(uint32_t count = 1, float time = 0.0f, uint32_t maxParticles = 500);

		// Particle Spawn 阶段标准模块 (初始化与几何分布)
		static VfxModule CreateInitParticleModule(float lifetime = 2.0f, float sizeStart = 0.3f, float sizeEnd = 0.05f);
		static VfxModule CreateConeSpawnModule();
		static VfxModule CreateSphereSpawnModule();
		static VfxModule CreateBoxSpawnModule();
		static VfxModule CreatePointSpawnModule();
		static VfxModule CreateRingSpawnModule(float radius = 1.5f, float initialSpeed = 0.0f);

		// Update 阶段标准模块
		static VfxModule CreateGravityModule();
		static VfxModule CreateLinearDragModule();
		static VfxModule CreateCurlNoiseModule();
		static VfxModule CreatePointAttractorModule();
		static VfxModule CreateVortexModule();
		static VfxModule CreateSolveVelocityModule();
		static VfxModule CreateScaleOverLifeModule(float startScale = 1.0f, float endScale = 0.0f);
		static VfxModule CreateColorOverLifeModule(const glm::vec4& colorStart = glm::vec4(1.0f), const glm::vec4& colorEnd = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
		static VfxModule CreateOrbitModule(float speed = 3.0f, const glm::vec3& axis = { 0.0f, 1.0f, 0.0f }, const glm::vec3& center = { 0.0f, 0.0f, 0.0f });
		static VfxModule CreateJitterModule(float jitterStrength = 2.0f, float jitterSpeed = 20.0f);

		// Render 阶段标准模块 (渲染器与材质设定)
				static VfxModule CreateRotationOverLifeModule(float rotSpeed = 1.5f, float initRot = 0.0f);
		static VfxModule CreateVortexTornadoModule(float vortexStrength = 6.0f, float upwardBuoyancy = 4.0f, float coneExpansion = 0.5f);
		static VfxModule CreateShockwaveExpansionModule(float expansionSpeed = 5.0f, float maxRadius = 8.0f, float dissipationRate = 1.2f);
		static VfxModule CreateFlipbookModule(uint32_t tilesX = 4, uint32_t tilesY = 4, float fps = 16.0f, bool loop = true);

		static VfxModule CreateSpriteRendererModule(bool additive = true, bool horizontal = false, float emissive = 2.0f);

		// 自定义 Slang 代码块工厂
		static VfxModule CreateCustomSlangModule(const std::string& customName = "CustomSimulationBlock");

		// 默认工业级标准发射器模块堆栈 (Cone + Gravity + Drag + Noise + Solve)
		static std::vector<VfxModule> CreateDefaultEmitterModuleStack();

		// 项目全局自定义模块持久化与库管理 (Project-Global Custom Slang Modules)
		static std::filesystem::path GetProjectModulesDirectory();
		static std::vector<VfxModule> LoadProjectCustomModules();
		static bool SaveProjectCustomModule(const VfxModule& mod);
		static bool DeleteProjectCustomModule(const std::string& moduleIdOrName);
	};
}
