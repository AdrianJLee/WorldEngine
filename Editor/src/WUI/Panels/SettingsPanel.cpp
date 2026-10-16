#include "wldpch.h"
#include "WUI/Panels/SettingsPanel.h"

#include "WUI/Shell/SettingsUi.h"
#include "World/Physics/PhysicsSettings.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Settings/SettingsRegistry.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace World
{
	namespace
	{
		const uint32_t kShadowMapSizes[] = { 512u, 1024u, 2048u, 4096u };

		std::string FormatFloat(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
			return buffer;
		}

		std::string BoolText(bool value) { return value ? "true" : "false"; }

		double SettingsNowSeconds()
		{
			return std::chrono::duration<double>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}

	// P4-UX12:面板整体迁到**设置注册表**(与 Editor Preferences 同一套渲染器)。
	//
	// 为什么迁移:手写行要为每一行重复"标签 + 控件 + tooltip + 无障碍节点 + 复位 + 重启徽标",
	// 于是"说明只存在于鼠标悬停里,脚本读不到""新增设置忘了加复位""徽标口径不一致"反复出现。
	// 注册表渲染器一次把这些做全;这里只负责:描述符(Id/类型/范围/生效时机/说明/绑定)+ 保存时机。
	//
	// 节点 id 规则唯一:`settings.<Id>`(P4-U12 删掉了 `settings3d.*` 这类老 id 覆盖)。
	// 本地化键同样用 `settings.<id>`。
	void SettingsPanel::RegisterProjectSettings()
	{
		using Settings::SettingApply;
		using Settings::SettingDescriptor;
		using Settings::SettingOption;
		using Settings::SettingsRegistry;
		using Settings::SettingScope;
		using Settings::SettingType;
		using ReadFn = std::function<std::string()>;
		using WriteFn = std::function<bool(const std::string&, std::string*)>;
		using FlagFn = std::function<bool()>;

		SettingsRegistry& registry = SettingsRegistry::Get();
		registry.ClearScope(SettingScope::Project);   // 幂等:面板重建时重新注册

		auto describe = [](const char* id, const char* group, SettingType type, SettingApply apply,
			const char* label, const char* tooltip)
		{
			SettingDescriptor descriptor;
			descriptor.Id = id;
			descriptor.Group = group;
			descriptor.Type = type;
			descriptor.Scope = SettingScope::Project;
			descriptor.Apply = apply;
			descriptor.Label = label;
			descriptor.Tooltip = tooltip;
			return descriptor;
		};

		auto addBool = [&registry, &describe](const char* id, const char* group, const char* label,
			const char* tooltip, ReadFn read, WriteFn write, FlagFn isDefault,
			FlagFn reset = FlagFn {})
		{
			SettingDescriptor descriptor = describe(id, group, SettingType::Bool, SettingApply::Immediate,
				label, tooltip);
			descriptor.Read = std::move(read);
			descriptor.Write = std::move(write);
			descriptor.IsDefault = std::move(isDefault);
			descriptor.Reset = std::move(reset);
			registry.Register(std::move(descriptor));
		};

		auto addNumber = [&registry, &describe](const char* id, const char* group, SettingType type,
			SettingApply apply, const char* label, const char* tooltip, const char* unit,
			double min, double max, double step, ReadFn read, WriteFn write, FlagFn isDefault, FlagFn reset)
		{
			SettingDescriptor descriptor = describe(id, group, type, apply, label, tooltip);
			descriptor.Unit = unit;
			descriptor.Min = min;
			descriptor.Max = max;
			descriptor.Step = step;
			descriptor.Read = std::move(read);
			descriptor.Write = std::move(write);
			descriptor.IsDefault = std::move(isDefault);
			descriptor.Reset = std::move(reset);
			registry.Register(std::move(descriptor));
		};

		auto addEnum = [&registry, &describe](const char* id, const char* group, SettingApply apply,
			const char* label, const char* tooltip, std::vector<SettingOption> options,
			ReadFn read, WriteFn write, FlagFn isDefault, FlagFn reset)
		{
			SettingDescriptor descriptor = describe(id, group, SettingType::Enum, apply, label, tooltip);
			descriptor.Options = std::move(options);
			descriptor.Read = std::move(read);
			descriptor.Write = std::move(write);
			descriptor.IsDefault = std::move(isDefault);
			descriptor.Reset = std::move(reset);
			registry.Register(std::move(descriptor));
		};

		// static:描述符的 lambda 会长期持有它们,不要捕获(捕获会在面板重建后悬空)。
		static const Asset::RenderingSettings renderDefaults;
		static const Asset::PhysicsSettingsData physicsDefaults;

		// ---- 渲染 Rendering ----
		addBool("culling", "Rendering", "Frustum Culling",
			"视锥剔除\n不在相机视野内的物体不提交绘制。\n默认:开。立即生效。",
			[this] { return BoolText(m_Edit.Culling); },
			[this](const std::string& value, std::string*) { m_Edit.Culling = value == "true"; return true; },
			[this] { return m_Edit.Culling == renderDefaults.Culling; },
			[this] { m_Edit.Culling = renderDefaults.Culling; return true; });

		addBool("shadows", "Rendering", "Directional Shadows",
			"平行光阴影\n主方向光渲染一张阴影贴图。\n默认:开。立即生效。",
			[this] { return BoolText(m_Edit.Shadows); },
			[this](const std::string& value, std::string*) { m_Edit.Shadows = value == "true"; return true; },
			[this] { return m_Edit.Shadows == renderDefaults.Shadows; },
			[this] { m_Edit.Shadows = renderDefaults.Shadows; return true; });

		addBool("vsync", "Rendering", "Vertical Sync",
			"垂直同步\n等待显示器刷新再呈现(防撕裂;会把帧率压到刷新率)。\n默认:开。立即生效(会重建交换链)。",
			[this] { return BoolText(m_Edit.Vsync); },
			[this](const std::string& value, std::string*) { m_Edit.Vsync = value == "true"; return true; },
			[this] { return m_Edit.Vsync == renderDefaults.Vsync; },
			[this] { m_Edit.Vsync = renderDefaults.Vsync; return true; });

		addEnum("shadow_map", "Rendering", SettingApply::Restart, "Shadow Map Size",
			"阴影贴图尺寸\n平行光阴影贴图的分辨率(512-4096,2 的幂)。\n下次启动生效。",
			{ SettingOption { "512", "512" }, SettingOption { "1024", "1024" },
				SettingOption { "2048", "2048" }, SettingOption { "4096", "4096" } },
			[this] { return std::to_string(m_Edit.ShadowMapSize); },
			[this](const std::string& value, std::string*) { m_Edit.ShadowMapSize = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.ShadowMapSize == renderDefaults.ShadowMapSize; },
			[this] { m_Edit.ShadowMapSize = renderDefaults.ShadowMapSize; return true; });

		addNumber("max_directional", "Rendering", SettingType::Int, SettingApply::Immediate, "Max Directional Lights",
			"平行光上限\n一帧打包多少盏平行光(1-2,与点光共享 8 盏的预算)。\n立即生效。",
			"", 1.0, static_cast<double>(Renderer3D::MaxDirectionalLightCapacity), 1.0,
			[this] { return std::to_string(m_Edit.MaxDirectionalLights); },
			[this](const std::string& value, std::string*) { m_Edit.MaxDirectionalLights = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.MaxDirectionalLights == renderDefaults.MaxDirectionalLights; },
			[this] { m_Edit.MaxDirectionalLights = renderDefaults.MaxDirectionalLights; return true; });

		addNumber("max_point", "Rendering", SettingType::Int, SettingApply::Immediate, "Max Point Lights",
			"点光上限\n一帧打包多少盏点光(0-7,与平行光共享 8 盏的预算)。\n立即生效。",
			"", 0.0, static_cast<double>(Renderer3D::MaxPointLightCapacity), 1.0,
			[this] { return std::to_string(m_Edit.MaxPointLights); },
			[this](const std::string& value, std::string*) { m_Edit.MaxPointLights = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.MaxPointLights == renderDefaults.MaxPointLights; },
			[this] { m_Edit.MaxPointLights = renderDefaults.MaxPointLights; return true; });

		addNumber("anisotropy", "Rendering", SettingType::Int, SettingApply::Immediate, "Texture Anisotropy",
			"纹理各向异性\n场景贴图的最大各向异性(1-16,夹到设备上限,1 = 关闭)。\n立即生效。",
			"", 1.0, 16.0, 1.0,
			[this] { return std::to_string(m_Edit.Anisotropy); },
			[this](const std::string& value, std::string*) { m_Edit.Anisotropy = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.Anisotropy == renderDefaults.Anisotropy; },
			[this] { m_Edit.Anisotropy = renderDefaults.Anisotropy; return true; });

		addNumber("render_scale", "Rendering", SettingType::Float, SettingApply::Immediate, "Render Scale",
			"渲染分辨率倍率\n只缩放场景渲染目标(0.25-2.0),窗口与 UI 不变。\n立即生效。",
			"x", 0.25, 2.0, 0.05,
			[this] { return FormatFloat(m_Edit.RenderScale); },
			[this](const std::string& value, std::string*) { m_Edit.RenderScale = static_cast<float>(std::atof(value.c_str())); return true; },
			[this] { return m_Edit.RenderScale == renderDefaults.RenderScale; },
			[this] { m_Edit.RenderScale = renderDefaults.RenderScale; return true; });

		addEnum("msaa", "Rendering", SettingApply::Restart, "MSAA (restart)",
			"多重采样\n场景通道的采样数(1/2/4/8)。管线与预览通道在启动时创建,需要重启。",
			{ SettingOption { "1", "1" }, SettingOption { "2", "2" },
				SettingOption { "4", "4" }, SettingOption { "8", "8" } },
			[this] { return std::to_string(m_Edit.Msaa); },
			[this](const std::string& value, std::string*) { m_Edit.Msaa = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.Msaa == renderDefaults.Msaa; },
			[this] { m_Edit.Msaa = renderDefaults.Msaa; return true; });

		addBool("instancing", "Rendering", "GPU Instancing",
			"GPU 实例合批\n同网格 + 同材质的物体合批绘制(重复道具收益最大)。\n默认:开。立即生效;关掉可做 A/B 对照。",
			[this] { return BoolText(m_Edit.Instancing); },
			[this](const std::string& value, std::string*) { m_Edit.Instancing = value == "true"; return true; },
			[this] { return m_Edit.Instancing == renderDefaults.Instancing; },
			[this] { m_Edit.Instancing = renderDefaults.Instancing; return true; });

		addBool("gpu_timing", "Rendering", "GPU Timing",
			"GPU 计时\n用时间戳查询统计场景通道耗时,Stats 面板与 AI 通道可读 gpuMs。\n默认:关(读回有一点代价)。立即生效。",
			[this] { return BoolText(m_Edit.GpuTiming); },
			[this](const std::string& value, std::string*) { m_Edit.GpuTiming = value == "true"; return true; },
			[this] { return m_Edit.GpuTiming == renderDefaults.GpuTiming; },
			[this] { m_Edit.GpuTiming = renderDefaults.GpuTiming; return true; });

		// ---- 物理 Physics ----
		addNumber("physics.fixed_step", "Physics", SettingType::Int, SettingApply::NextPlay, "Fixed Timestep",
			"固定步长\n物理更新频率 Hz(1-240)。下次 Play / Runtime 启动生效。\n默认:60。", "Hz", 1.0, 240.0, 1.0,
			[this] { return std::to_string(m_Physics.FixedStepHz); },
			[this](const std::string& value, std::string*) { m_Physics.FixedStepHz = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Physics.FixedStepHz == physicsDefaults.FixedStepHz; },
			[this] { m_Physics.FixedStepHz = physicsDefaults.FixedStepHz; return true; });

		addNumber("physics.gravity", "Physics", SettingType::Float, SettingApply::NextPlay, "Gravity",
			"重力\nY 轴加速度(m/s²)。下次 Play / Runtime 启动生效。\n默认:-9.81。", "", -50.0, 50.0, 0.1,
			[this] { return FormatFloat(m_Physics.Gravity); },
			[this](const std::string& value, std::string*) { m_Physics.Gravity = static_cast<float>(std::atof(value.c_str())); return true; },
			[this] { return m_Physics.Gravity == physicsDefaults.Gravity; },
			[this] { m_Physics.Gravity = physicsDefaults.Gravity; return true; });

				// ---- 大世界空间分区与关卡流送 Streaming (project.we.yaml 的 `streaming:` 区块) ----
		addEnum("streaming.cell_size", "Streaming", SettingApply::Immediate, "Default Cell Size",
			"默认分块尺寸\n大世界关卡空间分区的默认分块边长(米)。\n"
			"高密主城/室内建议 16m/32m;开阔大世界建议 64m/128m;超大平原建议 256m。",
			{ SettingOption { "16", "16m (高密主城/室内)" }, SettingOption { "32", "32m (紧凑城镇)" },
				SettingOption { "64", "64m (默认标准)" }, SettingOption { "128", "128m (开阔原野)" },
				SettingOption { "256", "256m (超大平原/海域)" } },
			[this] { return std::to_string(m_Streaming.CellSize); },
			[this](const std::string& value, std::string*)
			{
				m_Streaming.CellSize = static_cast<uint32_t>(std::atoi(value.c_str()));
				m_StreamingChanged = true;
				m_PendingSave = true;
				m_LastChangeSeconds = SettingsNowSeconds();
				return true;
			},
			[this] { return m_Streaming.CellSize == 64; },
			[this] { m_Streaming.CellSize = 64; m_StreamingChanged = true; return true; });

		addEnum("streaming.plane", "Streaming", SettingApply::Immediate, "Grid Plane",
			"分块基准切面\n大世界空间网格的切面基准(3D XZ / 2D XY / 2D ZY / 2.5D 等轴测 / 纯 3D 立体)。\n"
			"3D大世界选水平 XZ;2D横版选垂直 XY;侧面选垂直 ZY;暗黑/哈迪斯选等轴测;太空/天坑选纯 3D。",
			{
				SettingOption { "HorizontalXZ", "3D 水平 XZ (大世界 [默认])" },
				SettingOption { "VerticalXY", "2D 正面立面 XY (横版卷轴)" },
				SettingOption { "VerticalZY", "2D 侧面立面 ZY (纵向剖面)" },
				SettingOption { "Isometric", "2.5D 等轴测 (45°斜向菱形)" },
				SettingOption { "Full3D", "纯 3D 立体切块 (立方体体素)" }
			},
			[this] { return m_Streaming.Plane; },
			[this](const std::string& value, std::string*)
			{
				m_Streaming.Plane = value;
				m_StreamingChanged = true;
				m_PendingSave = true;
				m_LastChangeSeconds = SettingsNowSeconds();
				return true;
			},
			[this] { return m_Streaming.Plane == "HorizontalXZ"; },
			[this] { m_Streaming.Plane = "HorizontalXZ"; m_StreamingChanged = true; return true; });

		addNumber("streaming.view_distance", "Streaming", SettingType::Float, SettingApply::Immediate,
			"Loading Radius (m)", "流送加载半径\n以玩家/相机为中心动态加载 Chunk 的视野距离(米)。",
			"m", 32.0, 1000.0, 16.0,
			[this] { return FormatFloat(m_Streaming.ViewDistance); },
			[this](const std::string& value, std::string*)
			{
				m_Streaming.ViewDistance = std::clamp(static_cast<float>(std::atof(value.c_str())), 32.0f, 1000.0f);
				if (m_Streaming.UnloadDistance <= m_Streaming.ViewDistance)
					m_Streaming.UnloadDistance = m_Streaming.ViewDistance + 32.0f;
				m_StreamingChanged = true;
				m_PendingSave = true;
				m_LastChangeSeconds = SettingsNowSeconds();
				return true;
			},
			[this] { return std::fabs(m_Streaming.ViewDistance - 256.0f) < 1e-3f; },
			[this] { m_Streaming.ViewDistance = 256.0f; m_StreamingChanged = true; return true; });

		// ---- 启动与内容 Startup(写清单是即时的,所以不给"恢复默认":它们不是"默认值"问题) ----
		addEnum("project.renderer", "Startup", SettingApply::Restart, "Renderer",
			"渲染后端\n编辑器与 Runtime 启动时使用的 RHI 后端(OpenGL / Vulkan)。\n"
			"运行中热切换会串资源,因此需要重启编辑器;改完面板会给「立即重启」入口。",
			{ SettingOption { "opengl", "OpenGL" }, SettingOption { "vulkan", "Vulkan" } },
			[this] { return m_Startup.Renderer; },
			[this](const std::string& value, std::string*)
			{
				std::string message;
				if (!m_Host || !m_Host->SaveProjectStartupSettings(value, m_Startup.StartScene,
					m_Startup.ContentRoot, &message))
					return false;
				m_Startup.Renderer = value;
				return true;
			},
			[this] { return m_Startup.Renderer == "opengl"; },
			FlagFn {});

		std::vector<SettingOption> sceneOptions;
		for (const std::string& scene : m_SceneOptions)
			sceneOptions.push_back(SettingOption { scene, scene });
		if (sceneOptions.empty() && !m_Startup.StartScene.empty())
			sceneOptions.push_back(SettingOption { m_Startup.StartScene, m_Startup.StartScene });
		// 场景可能很多 → 用可搜索下拉(注册表描述符的 Searchable 开关)。
		{
			SettingDescriptor startScene = describe("project.start_scene", "Startup", SettingType::Enum,
				SettingApply::NextPlay, "Start Scene",
				"启动场景\n打包后的 Runtime 启动时加载的场景(路径相对内容根)。\n下次启动 Runtime / 打包时生效。");
			startScene.Searchable = true;
			startScene.Options = std::move(sceneOptions);
			startScene.Read = [this] { return m_Startup.StartScene; };
			startScene.Write = [this](const std::string& value, std::string*)
			{
				std::string message;
				if (!m_Host || !m_Host->SaveProjectStartupSettings(m_Startup.Renderer, value,
					m_Startup.ContentRoot, &message))
					return false;
				m_Startup.StartScene = value;
				return true;
			};
			startScene.IsDefault = [this] { return m_Startup.StartScene.empty(); };
			registry.Register(std::move(startScene));
		}

		// 内容根:文本行(注册表渲染器会给它 180 宽 + 单位位),回车提交。
		{
			SettingDescriptor descriptor = describe("project.content_root", "Startup", SettingType::Path,
				SettingApply::Restart, "Content Root",
				"内容根\n存放项目资产的目录(相对 project.we.yaml)。\n挂载点在启动时建立,改完需要重启编辑器。");
			descriptor.Read = [this] { return m_Startup.ContentRoot; };
			descriptor.Write = [this](const std::string& value, std::string*)
			{
				if (value.empty())
					return false;
				std::string message;
				if (!m_Host || !m_Host->SaveProjectStartupSettings(m_Startup.Renderer, m_Startup.StartScene,
					value, &message))
					return false;
				m_Startup.ContentRoot = value;
				return true;
			};
			descriptor.IsDefault = [this] { return m_Startup.ContentRoot == "assets"; };
			registry.Register(std::move(descriptor));
		}
	}

	// P4-U4(2026-09-21):Asset Import Defaults —— project.we.yaml 的 `imports:`。
	//
	// 语义(与资产自描述的分工):**已有资产的 meta 逐源优先**;这里改的是"还没有产物"时的默认,
	// 所以它只影响之后的新导入,不会悄悄改变已有资产。
	void SettingsPanel::RegisterImportSettings()
	{
		using Settings::SettingApply;
		using Settings::SettingDescriptor;
		using Settings::SettingOption;
		using Settings::SettingsRegistry;
		using Settings::SettingScope;
		using Settings::SettingType;
		using ReadFn = std::function<std::string()>;
		using WriteFn = std::function<bool(const std::string&, std::string*)>;
		using FlagFn = std::function<bool()>;

		SettingsRegistry& registry = SettingsRegistry::Get();
		registry.ClearScope(SettingScope::Import);

		// 描述符长期持有这些 lambda:只捕获 this,不捕获局部量(面板重建后不会悬空)。
		static const Asset::ModelImportSettings importDefaults;
		const auto sameAsDefault = [this](const std::function<bool(const Asset::ModelImportSettings&,
			const Asset::ModelImportSettings&)>& compare)
		{
			return FlagFn([this, compare] { return compare(m_Import, importDefaults); });
		};

		auto describe = [](const char* id, SettingType type, SettingApply apply, const char* label,
			const char* tooltip, const char* unit, double min, double max, double step)
		{
			SettingDescriptor descriptor;
			descriptor.Id = id;
			descriptor.Group = "Model";
			descriptor.Type = type;
			descriptor.Scope = SettingScope::Import;
			descriptor.Apply = apply;
			descriptor.Label = label;
			descriptor.Tooltip = tooltip;
			descriptor.Unit = unit;
			descriptor.Min = min;
			descriptor.Max = max;
			descriptor.Step = step;
			return descriptor;
		};
		auto registerRow = [&registry](SettingDescriptor descriptor, ReadFn read, WriteFn write,
			FlagFn isDefault, FlagFn reset)
		{
			descriptor.Read = std::move(read);
			descriptor.Write = std::move(write);
			descriptor.IsDefault = std::move(isDefault);
			descriptor.Reset = std::move(reset);
			registry.Register(std::move(descriptor));
		};

		// 缩放(导入期烘进几何/节点:1 = 原尺寸)
		registerRow(describe("import.model.scale", SettingType::Float, SettingApply::Immediate,
				"Model Scale", "模型缩放\n导入期把源放大/缩小后烘进几何与节点(不是运行时缩放)。\n默认:1.0。下次导入生效。",
				"x", 0.001, 1000.0, 0.1),
			[this] { return std::to_string(m_Import.Scale); },
			[this](const std::string& value, std::string* error)
			{
				const float parsed = std::strtof(value.c_str(), nullptr);
				if (!std::isfinite(parsed) || parsed <= 0.0f)
				{
					if (error) *error = Wui::Tr("settings.import.model.scale.error",
						"Scale must be a positive number");
					return false;
				}
				m_Import.Scale = parsed;
				return true;
			},
			sameAsDefault([](const Asset::ModelImportSettings& a, const Asset::ModelImportSettings& b)
				{ return std::fabs(a.Scale - b.Scale) < 1e-4f; }),
			[this] { m_Import.Scale = importDefaults.Scale; return true; });

		// 上轴(Y = 引擎坐标;Z = 绕 X 轴 -90° 烘焙)
		{
			SettingDescriptor descriptor = describe("import.model.up_axis", SettingType::Enum,
				SettingApply::Immediate, "Source Up Axis",
				"源上轴\nY = 已经是引擎坐标;Z = 导入期绕 X 轴 -90° 烘焙(Blender/多数 DCC 的默认)。\n默认:Y。",
				"", 0.0, 0.0, 0.0);
			descriptor.Options = {
				{ "y", "Y (engine)" },
				{ "z", "Z (convert)" },
			};
			registerRow(std::move(descriptor),
				[this] { return m_Import.UpAxis == 1 ? "z" : "y"; },
				[this](const std::string& value, std::string*)
				{
					m_Import.UpAxis = value == "z" ? 1 : 0;
					return true;
				},
				sameAsDefault([](const Asset::ModelImportSettings& a, const Asset::ModelImportSettings& b)
					{ return a.UpAxis == b.UpAxis; }),
				[this] { m_Import.UpAxis = importDefaults.UpAxis; return true; });
		}

		struct BoolRow { const char* Id; const char* Label; const char* Tooltip; bool Asset::ModelImportSettings::* Field; };
		static const BoolRow boolRows[] = {
			{ "import.model.export_materials", "Export Materials",
				"导出材质\n导入时为本模型产出/复用 .wmat。关掉=材质槽留空。\n默认:开。",
				&Asset::ModelImportSettings::ExportMaterials },
			{ "import.model.export_textures", "Export Textures",
				"导出贴图\n把 glTF 内嵌/引用的贴图落到内容目录。关掉=材质贴图路径留空。\n默认:开。",
				&Asset::ModelImportSettings::ExportTextures },
			{ "import.model.import_animations", "Import Animations",
				"导入动画\n读取 glTF animations 并烘成固定采样率的关键帧。\n默认:开。",
				&Asset::ModelImportSettings::ImportAnimations },
			{ "import.model.import_skins", "Import Skins",
				"导入骨架\n关掉时蒙皮网格按静态处理(骨架/蒙皮数据不落盘)。\n默认:开。",
				&Asset::ModelImportSettings::ImportSkins },
			{ "import.model.generate_normals", "Generate Normals",
				"补法线\n源缺法线时按面法线补齐;关掉则缺法线直接报错。\n默认:开。",
				&Asset::ModelImportSettings::GenerateNormals },
			{ "import.model.reuse_materials", "Reuse Materials",
				"复用材质\n导入前算内容哈希,目标目录已有同内容材质就复用它的路径(否则每个模型一份副本)。\n默认:开。",
				&Asset::ModelImportSettings::ReuseMaterials },
			{ "import.model.reuse_textures", "Reuse Textures",
				"复用贴图\n同材质复用的口径:同内容贴图复用同一份文件。\n默认:开。",
				&Asset::ModelImportSettings::ReuseTextures },
		};
		for (const BoolRow& row : boolRows)
		{
			// 注意:这里不能写 `const bool ModelImportSettings::*`(那会变成"指向 const 成员的指针",
			// 赋值被拒 C3892)。成员本身非 const,const 只在 const 行数组上。
			bool Asset::ModelImportSettings::* field = row.Field;
			registerRow(describe(row.Id, SettingType::Bool, SettingApply::Immediate, row.Label, row.Tooltip,
					"", 0.0, 0.0, 0.0),
				[this, field] { return (m_Import.*field) ? "true" : "false"; },
				[this, field](const std::string& value, std::string*)
				{
					m_Import.*field = value == "true";
					return true;
				},
				FlagFn([this, field] { return (m_Import.*field) == (importDefaults.*field); }),
				FlagFn([this, field] { m_Import.*field = importDefaults.*field; return true; }));
		}

		// 动画采样率(Hz)
		registerRow(describe("import.model.animation_sample_rate", SettingType::Float,
				SettingApply::Immediate, "Animation Sample Rate",
				"动画采样率\n导入期把 glTF 关键帧烘成等间隔关键帧(运行时不再解析插值器)。\n范围 1–120 Hz,默认 30。",
				"Hz", 1.0, 120.0, 1.0),
			[this] { return std::to_string(m_Import.AnimationSampleRate); },
			[this](const std::string& value, std::string* error)
			{
				const float parsed = std::strtof(value.c_str(), nullptr);
				if (!std::isfinite(parsed) || parsed < 1.0f || parsed > 120.0f)
				{
					if (error) *error = Wui::Tr("settings.import.model.animation_sample_rate.error",
						"Sample rate must be in [1, 120] Hz");
					return false;
				}
				m_Import.AnimationSampleRate = parsed;
				return true;
			},
			sameAsDefault([](const Asset::ModelImportSettings& a, const Asset::ModelImportSettings& b)
				{ return std::fabs(a.AnimationSampleRate - b.AnimationSampleRate) < 1e-4f; }),
			[this] { m_Import.AnimationSampleRate = importDefaults.AnimationSampleRate; return true; });

		// 共享材质查找目录(相对内容根;空 = 只在导入目的地里找)
		{
			SettingDescriptor descriptor = describe("import.model.shared_material_folder", SettingType::Text,
				SettingApply::Immediate, "Shared Material Folder",
				"共享材质目录\n复用查找的额外范围(相对内容根,如 materials/shared)。\n空 = 只在本次导入的目的地目录里找。",
				"", 0.0, 0.0, 0.0);
			registerRow(std::move(descriptor),
				[this] { return m_Import.SharedMaterialFolder; },
				[this](const std::string& value, std::string*)
				{
					m_Import.SharedMaterialFolder = value;
					return true;
				},
				sameAsDefault([](const Asset::ModelImportSettings& a, const Asset::ModelImportSettings& b)
					{ return a.SharedMaterialFolder == b.SharedMaterialFolder; }),
				[this] { m_Import.SharedMaterialFolder = importDefaults.SharedMaterialFolder; return true; });
		}
	}

	// P4-U4(2026-09-21):World / Scene Settings —— 关卡 manifest 的 `World:` 块。
	//
	// 只放"引擎已经有实现、但过去只能靠环境变量或全局设置"的 knob(符合项目口径:
	// 功能没实现就不进面板)。读写直接绑**当前文档场景**,改完标脏,落盘随"保存场景"。
	void SettingsPanel::RegisterWorldSettings()
	{
		using Settings::SettingApply;
		using Settings::SettingDescriptor;
		using Settings::SettingsRegistry;
		using Settings::SettingScope;
		using Settings::SettingType;
		using ReadFn = std::function<std::string()>;
		using WriteFn = std::function<bool(const std::string&, std::string*)>;

		SettingsRegistry& registry = SettingsRegistry::Get();
		registry.ClearScope(SettingScope::World);

		// 取当前文档场景;没有场景(未打开关卡)时全部行禁用并写清原因 —— 而不是假装可改。
		const auto worldOf = [this]() -> Scene* 
		{
			if (!m_Host)
				return nullptr;
			const Ref<Scene> scene = m_Host->GetActiveScene();
			return scene.get();
		};

		// 物理调试线框:过去只有 WLD_PHYSICS_DEBUG 环境变量
		{
			SettingDescriptor descriptor;
			descriptor.Id = "world.physics.debug";
			descriptor.Group = "Physics";
			descriptor.Type = SettingType::Bool;
			descriptor.Scope = SettingScope::World;
			descriptor.Apply = SettingApply::Immediate;
			descriptor.Label = "Physics Debug Draw";
			descriptor.Tooltip = "物理调试线框\n在视口里画出碰撞体/接触点的世界空间线段\n"
				"(等价于环境变量 WLD_PHYSICS_DEBUG=1,但按场景保存)。\n默认:关。进入 Play/Simulate 后可见。";
			descriptor.Read = [worldOf]()
			{
				const Scene* scene = worldOf();
				return (scene && scene->GetWorldSettings().PhysicsDebug) ? "true" : "false";
			};
			descriptor.Write = [this, worldOf](const std::string& value, std::string* error)
			{
				Scene* scene = worldOf();
				if (!scene)
				{
					if (error) *error = Wui::Tr("settings.world.no_scene",
						"No scene is open — open a .wlevel first");
					return false;
				}
				scene->GetWorldSettings().PhysicsDebug = value == "true";
				return true;
			};
			descriptor.IsDefault = [worldOf]
			{
				const Scene* scene = worldOf();
				return !scene || !scene->GetWorldSettings().PhysicsDebug;
			};
			descriptor.Reset = [this, worldOf]
			{
				Scene* scene = worldOf();
				if (!scene) return false;
				scene->GetWorldSettings().PhysicsDebug = false;
				return true;
			};
			descriptor.IsEnabled = [worldOf] { return worldOf() != nullptr; };
			descriptor.DisabledReason = "No scene is open — open a .wlevel first";
			registry.Register(std::move(descriptor));
		}

		// 重力覆盖:模式(跟随项目/自定义)+ 值。值行只在自定义时可用。
		SettingsRegistry& registryForMode = registry;
		auto modeIsCustom = [worldOf]()
		{
			const Scene* scene = worldOf();
			return scene && scene->GetWorldSettings().HasGravityOverride();
		};
		{
			SettingDescriptor descriptor;
			descriptor.Id = "world.physics.gravity_mode";
			descriptor.Group = "Physics";
			descriptor.Type = SettingType::Enum;
			descriptor.Scope = SettingScope::World;
			descriptor.Apply = SettingApply::NextPlay;
			descriptor.Label = "Scene Gravity";
			descriptor.Tooltip = "场景重力\n跟随项目 = 用 project.we.yaml 的 physics.gravity;\n"
				"自定义 = 只覆盖当前场景(存在关卡 manifest 里)。\n默认:跟随项目。进入 Play 时生效。";
			descriptor.Options = {
				{ "project", "Follow project" },
				{ "custom", "Custom" },
			};
			descriptor.Read = [modeIsCustom] { return modeIsCustom() ? "custom" : "project"; };
			descriptor.Write = [this, worldOf](const std::string& value, std::string* error)
			{
				Scene* scene = worldOf();
				if (!scene)
				{
					if (error) *error = Wui::Tr("settings.world.no_scene",
						"No scene is open — open a .wlevel first");
					return false;
				}
				if (value == "custom")
				{
					// 起点 = 项目重力,用户改数值时不会突然跳到 0。
					if (!scene->GetWorldSettings().HasGravityOverride())
						scene->GetWorldSettings().Gravity = PhysicsSettings::Get().Gravity;
				}
				else
				{
					scene->GetWorldSettings().Gravity = std::numeric_limits<float>::quiet_NaN();
				}
				return true;
			};
			descriptor.IsDefault = [modeIsCustom] { return !modeIsCustom(); };
			descriptor.Reset = [this, worldOf]
			{
				Scene* scene = worldOf();
				if (!scene) return false;
				scene->GetWorldSettings().Gravity = std::numeric_limits<float>::quiet_NaN();
				return true;
			};
			descriptor.IsEnabled = [worldOf] { return worldOf() != nullptr; };
			descriptor.DisabledReason = "No scene is open — open a .wlevel first";
			registryForMode.Register(std::move(descriptor));
		}
		{
			SettingDescriptor descriptor;
			descriptor.Id = "world.physics.gravity";
			descriptor.Group = "Physics";
			descriptor.Type = SettingType::Float;
			descriptor.Scope = SettingScope::World;
			descriptor.Apply = SettingApply::NextPlay;
			descriptor.Label = "Scene Gravity Value";
			descriptor.Tooltip = "场景重力值\nY 轴加速度(m/s²);只在上面的模式 = 自定义时生效。\n"
				"默认:跟随项目。进入 Play 时生效。";
			descriptor.Unit = "m/s²";
			descriptor.Min = -100.0;
			descriptor.Max = 100.0;
			descriptor.Step = 0.1;
			descriptor.Read = [worldOf, modeIsCustom]
			{
				const Scene* scene = worldOf();
				if (!scene)
					return std::string("0");
				if (!modeIsCustom())
					return std::to_string(PhysicsSettings::Get().Gravity);
				return std::to_string(scene->GetWorldSettings().Gravity);
			};
			descriptor.Write = [this, worldOf](const std::string& value, std::string* error)
			{
				Scene* scene = worldOf();
				if (!scene)
				{
					if (error) *error = Wui::Tr("settings.world.no_scene",
						"No scene is open — open a .wlevel first");
					return false;
				}
				const float parsed = std::strtof(value.c_str(), nullptr);
				if (!std::isfinite(parsed))
				{
					if (error) *error = Wui::Tr("settings.world.physics.gravity.error",
						"Gravity must be a finite number");
					return false;
				}
				scene->GetWorldSettings().Gravity = parsed;
				return true;
			};
			descriptor.IsEnabled = [modeIsCustom] { return modeIsCustom(); };
			descriptor.DisabledReason = "Gravity mode is 'Follow project' — switch it to Custom first";
			registry.Register(std::move(descriptor));
		}
	}

	void SettingsPanel::EnsureDefaultProfiles()
	{
		if (m_Physics.Channels.Profiles.empty())
		{
			m_Physics.Channels = Asset::CollisionChannelConfig::Default();
		}

		auto addProfileIfMissing = [this](std::string_view name, uint8_t objectType,
			Asset::CollisionResponse defaultResp,
			const std::vector<std::pair<uint8_t, Asset::CollisionResponse>>& overrides)
		{
			for (const auto& p : m_Physics.Channels.Profiles)
			{
				if (p.Name == name)
					return;
			}
			Asset::CollisionProfile prof;
			prof.Name = std::string(name);
			prof.ObjectType = objectType;
			prof.Responses.SetAll(defaultResp);
			for (const auto& ov : overrides)
				prof.Responses.SetResponse(ov.first, ov.second);
			m_Physics.Channels.Profiles.push_back(prof);
		};

		addProfileIfMissing("BlockAll", 0, Asset::CollisionResponse::Block, {});
		addProfileIfMissing("OverlapAll", 0, Asset::CollisionResponse::Overlap, {});
		addProfileIfMissing("Pawn", 2, Asset::CollisionResponse::Block, {
			{ 4, Asset::CollisionResponse::Overlap },
			{ 9, Asset::CollisionResponse::Overlap },
			{ 49, Asset::CollisionResponse::Ignore }
		});
		addProfileIfMissing("WorldStatic", 1, Asset::CollisionResponse::Block, {
			{ 4, Asset::CollisionResponse::Overlap }
		});
		addProfileIfMissing("Glass", 1, Asset::CollisionResponse::Block, {
			{ 48, Asset::CollisionResponse::Ignore },
			{ 49, Asset::CollisionResponse::Ignore }
		});
		addProfileIfMissing("InvisibleWall", 1, Asset::CollisionResponse::Ignore, {
			{ 2, Asset::CollisionResponse::Block },
			{ 7, Asset::CollisionResponse::Block }
		});
		addProfileIfMissing("TriggerOnly", 4, Asset::CollisionResponse::Ignore, {
			{ 2, Asset::CollisionResponse::Overlap },
			{ 7, Asset::CollisionResponse::Overlap }
		});
		addProfileIfMissing("NoCollision", 0, Asset::CollisionResponse::Ignore, {});
	}

	void SettingsPanel::RenderCollisionTab(Wui::WuiContext& ctx, const Wui::WuiRect& page,
		const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		EnsureDefaultProfiles();

		// 1. 顶部子模式导航 (Collision Matrix vs Collision Profiles)
		const float subTabH = 28.0f;
		const Wui::WuiRect subTabRect { page.X, page.Y, page.W, subTabH };
		std::vector<std::string> subTabs = {
			Wui::Tr("settings.collision.subtab.matrix", "Collision Matrix"),
			Wui::Tr("settings.collision.subtab.profiles", "Collision Profiles")
		};
		Wui::TabBar(ctx, Wui::HashId("settings.collision.subtabs"), subTabRect, subTabs, m_CollisionSubTab, theme);

		const Wui::WuiRect subPage { page.X, page.Y + 32.0f, page.W, std::max(60.0f, page.H - 32.0f) };
		if (m_CollisionSubTab == 1)
		{
			RenderCollisionProfiles(ctx, subPage, theme, host);
			return;
		}

		// 2. 收集激活具名通道 (分流 Object 0..47 与 Trace 48..63)
		std::vector<uint8_t> activeObjectChannels;
		std::vector<uint8_t> activeTraceChannels;
		std::vector<uint8_t> activeAllChannels;
		activeObjectChannels.reserve(Asset::CollisionChannelConfig::kTraceChannelStart);
		activeTraceChannels.reserve(Asset::CollisionChannelConfig::kMaxChannels - Asset::CollisionChannelConfig::kTraceChannelStart);
		activeAllChannels.reserve(Asset::CollisionChannelConfig::kMaxChannels);

		for (uint8_t i = 0; i < static_cast<uint8_t>(Asset::CollisionChannelConfig::kMaxChannels); ++i)
		{
			if (!m_Physics.Channels.ChannelNames[i].empty())
			{
				activeAllChannels.push_back(i);
				if (m_Physics.Channels.GetChannelType(i) == Asset::CollisionChannelType::Trace ||
					i >= Asset::CollisionChannelConfig::kTraceChannelStart)
				{
					activeTraceChannels.push_back(i);
				}
				else
				{
					activeObjectChannels.push_back(i);
				}
			}
		}
		if (activeObjectChannels.empty())
		{
			m_Physics.Channels.SetChannelName(0, "Default");
			m_Physics.Channels.SetChannelType(0, Asset::CollisionChannelType::Object);
			activeObjectChannels.push_back(0);
			activeAllChannels.push_back(0);
		}

		// 3. 工具栏第 1 行: 通道分类与导航过滤器 (Object / Trace / All)
		float curY = subPage.Y;
		float curX = subPage.X;

		const bool isObj = (m_ChannelFilter == 0);
		const bool isTrace = (m_ChannelFilter == 1);
		const bool isAll = (m_ChannelFilter == 2);

		if (Wui::ButtonEx(ctx, Wui::HashId("settings.collision.filter.obj"),
			{ curX, curY, 144.0f, 24.0f },
			Wui::Tr("settings.collision.filter.obj", "Object Channels (0..47)"), theme, true, isObj,
			Wui::Tr("settings.collision.filter.obj.tooltip",
				"Filter to Object channels (physical rigid bodies & collision shapes)")))
		{
			m_ChannelFilter = 0;
		}
		curX += 148.0f;

		if (Wui::ButtonEx(ctx, Wui::HashId("settings.collision.filter.trace"),
			{ curX, curY, 140.0f, 24.0f },
			Wui::Tr("settings.collision.filter.trace", "Trace Channels (48..63)"), theme, true, isTrace,
			Wui::Tr("settings.collision.filter.trace.tooltip",
				"Filter to Trace channels (Visibility, Camera, WeaponBullet, rays & sweep queries)")))
		{
			m_ChannelFilter = 1;
		}
		curX += 144.0f;

		if (Wui::ButtonEx(ctx, Wui::HashId("settings.collision.filter.all"),
			{ curX, curY, 116.0f, 24.0f },
			Wui::Tr("settings.collision.filter.all", "All Channels (0..63)"), theme, true, isAll,
			Wui::Tr("settings.collision.filter.all.tooltip",
				"Display full 64-channel cross-category matrix")))
		{
			m_ChannelFilter = 2;
		}
		curX += 124.0f;

		char countBuf[128] = {};
		std::snprintf(countBuf, sizeof(countBuf), "%d Active Channels (%d Object, %d Trace)",
			static_cast<int>(activeAllChannels.size()),
			static_cast<int>(activeObjectChannels.size()),
			static_cast<int>(activeTraceChannels.size()));
		Wui::Label(ctx, { curX + 4.0f, curY + 5.0f }, countBuf, theme.TextMuted, 12.0f);

		// 4. 工具栏第 2 行: 矩阵批量操作 (Reset Defaults / All Block / All Overlap / All Ignore / View Mode / Profiles)
		curY = subPage.Y + 28.0f;
		curX = subPage.X;

		if (Wui::Button(ctx, Wui::HashId("settings.collision.reset"),
			{ curX, curY, 104.0f, 24.0f },
			Wui::Tr("settings.collision.reset", "Reset Defaults"), theme))
		{
			m_Physics.Channels = Asset::CollisionChannelConfig::Default();
			EnsureDefaultProfiles();
			PhysicsSettings::Set(m_Physics);
			m_PendingSave = true;
			m_LastChangeSeconds = SettingsNowSeconds();
			m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
			m_StatusIsError = false;
		}
		Wui::Tooltip(ctx, { curX, curY, 104.0f, 24.0f },
			Wui::Tr("settings.collision.reset.tooltip",
				"Reset all collision channels, matrix, and profiles to factory defaults"));
		curX += 108.0f;

		if (Wui::Button(ctx, Wui::HashId("settings.collision.all_block"),
			{ curX, curY, 84.0f, 24.0f },
			Wui::Tr("settings.collision.all_block", "All Block"), theme))
		{
			const auto& targetCols = (m_ChannelFilter == 1)
				? (activeObjectChannels.empty() ? activeTraceChannels : activeObjectChannels)
				: ((m_ChannelFilter == 0) ? activeObjectChannels : activeAllChannels);
			const auto& targetRows = (m_ChannelFilter == 1) ? activeTraceChannels
				: ((m_ChannelFilter == 0) ? activeObjectChannels : activeAllChannels);

			for (uint8_t a : targetRows)
			{
				for (uint8_t b : targetCols)
					m_Physics.Channels.SetCollisionResponse(a, b, Asset::CollisionResponse::Block);
			}
			PhysicsSettings::Set(m_Physics);
			m_PendingSave = true;
			m_LastChangeSeconds = SettingsNowSeconds();
			m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
			m_StatusIsError = false;
		}
		Wui::Tooltip(ctx, { curX, curY, 84.0f, 24.0f },
			Wui::Tr("settings.collision.all_block.tooltip",
				"Set collision response between active channels to Block"));
		curX += 88.0f;

		if (Wui::Button(ctx, Wui::HashId("settings.collision.all_overlap"),
			{ curX, curY, 88.0f, 24.0f },
			Wui::Tr("settings.collision.all_overlap", "All Overlap"), theme))
		{
			const auto& targetCols = (m_ChannelFilter == 1)
				? (activeObjectChannels.empty() ? activeTraceChannels : activeObjectChannels)
				: ((m_ChannelFilter == 0) ? activeObjectChannels : activeAllChannels);
			const auto& targetRows = (m_ChannelFilter == 1) ? activeTraceChannels
				: ((m_ChannelFilter == 0) ? activeObjectChannels : activeAllChannels);

			for (uint8_t a : targetRows)
			{
				for (uint8_t b : targetCols)
					m_Physics.Channels.SetCollisionResponse(a, b, Asset::CollisionResponse::Overlap);
			}
			PhysicsSettings::Set(m_Physics);
			m_PendingSave = true;
			m_LastChangeSeconds = SettingsNowSeconds();
			m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
			m_StatusIsError = false;
		}
		Wui::Tooltip(ctx, { curX, curY, 88.0f, 24.0f },
			Wui::Tr("settings.collision.all_overlap.tooltip",
				"Set collision response between active channels to Overlap"));
		curX += 92.0f;

		if (Wui::Button(ctx, Wui::HashId("settings.collision.all_ignore"),
			{ curX, curY, 84.0f, 24.0f },
			Wui::Tr("settings.collision.all_ignore", "All Ignore"), theme))
		{
			const auto& targetCols = (m_ChannelFilter == 1)
				? (activeObjectChannels.empty() ? activeTraceChannels : activeObjectChannels)
				: ((m_ChannelFilter == 0) ? activeObjectChannels : activeAllChannels);
			const auto& targetRows = (m_ChannelFilter == 1) ? activeTraceChannels
				: ((m_ChannelFilter == 0) ? activeObjectChannels : activeAllChannels);

			for (uint8_t a : targetRows)
			{
				for (uint8_t b : targetCols)
					m_Physics.Channels.SetCollisionResponse(a, b, Asset::CollisionResponse::Ignore);
			}
			PhysicsSettings::Set(m_Physics);
			m_PendingSave = true;
			m_LastChangeSeconds = SettingsNowSeconds();
			m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
			m_StatusIsError = false;
		}
		Wui::Tooltip(ctx, { curX, curY, 84.0f, 24.0f },
			Wui::Tr("settings.collision.all_ignore.tooltip",
				"Set collision response between active channels to Ignore"));
		curX += 88.0f;

		const std::string viewLabel = m_CollisionTriangleOnly
			? Wui::Tr("settings.collision.view_triangle", "View: Triangle")
			: Wui::Tr("settings.collision.view_full", "View: Full Grid");
		if (Wui::Button(ctx, Wui::HashId("settings.collision.view_mode"),
			{ curX, curY, 116.0f, 24.0f }, viewLabel, theme))
		{
			m_CollisionTriangleOnly = !m_CollisionTriangleOnly;
		}
		Wui::Tooltip(ctx, { curX, curY, 116.0f, 24.0f },
			Wui::Tr("settings.collision.view_mode.tooltip",
				"Toggle between Full Matrix and Lower Triangle view (for symmetric Object/All matrices)"));
		curX += 120.0f;

		char profBtnBuf[64] = {};
		std::snprintf(profBtnBuf, sizeof(profBtnBuf), "Profiles (%d)…",
			static_cast<int>(m_Physics.Channels.Profiles.size()));
		if (Wui::Button(ctx, Wui::HashId("settings.collision.goto_profiles"),
			{ curX, curY, 120.0f, 24.0f }, profBtnBuf, theme))
		{
			m_CollisionSubTab = 1;
		}
		Wui::Tooltip(ctx, { curX, curY, 120.0f, 24.0f },
			Wui::Tr("settings.collision.goto_profiles.tooltip", "Switch to Collision Profiles selector & manager"));

		// 5. 内容区分割: 左侧通道列表, 右侧三态碰撞矩阵
		const float contentTop = subPage.Y + 56.0f;
		const float contentH = std::max(60.0f, subPage.H - 56.0f);
		const bool narrow = subPage.W < 640.0f;
		const float listW = narrow ? subPage.W : 260.0f;
		const float listH = narrow ? 160.0f : contentH;
		const Wui::WuiRect listRect { subPage.X, contentTop, listW, listH };

		const float matrixX = narrow ? subPage.X : subPage.X + listW + 12.0f;
		const float matrixY = narrow ? contentTop + listH + 8.0f : contentTop;
		const float matrixW = narrow ? subPage.W : subPage.W - listW - 12.0f;
		const float matrixH = narrow ? std::max(60.0f, contentH - listH - 8.0f) : contentH;
		const Wui::WuiRect matrixRect { matrixX, matrixY, matrixW, matrixH };

		// --- 通道列表 Channels ---
		{
			Wui::Label(ctx, { listRect.X + 2.0f, listRect.Y + 3.0f },
				Wui::Tr("settings.collision.channels", "Active Channels"), theme.Text, 13.0f);

			const std::string filterLabel = m_ShowAllChannels
				? Wui::Tr("settings.collision.show_all", "All Slots")
				: Wui::Tr("settings.collision.show_active", "Active");
			if (Wui::Button(ctx, Wui::HashId("settings.collision.ch.filter"),
				{ listRect.X + listRect.W - 136.0f, listRect.Y, 64.0f, 20.0f }, filterLabel, theme))
			{
				m_ShowAllChannels = !m_ShowAllChannels;
			}
			Wui::Tooltip(ctx, { listRect.X + listRect.W - 136.0f, listRect.Y, 64.0f, 20.0f },
				Wui::Tr("settings.collision.ch.filter.tooltip",
					"Toggle between active channels and all available slots in category"));

			// 查找当前分类下的下一个可用空闲槽位
			int nextFreeSlot = -1;
			if (m_ChannelFilter == 0) // Object 0..47
			{
				for (uint8_t i = 0; i < Asset::CollisionChannelConfig::kTraceChannelStart; ++i)
				{
					if (m_Physics.Channels.ChannelNames[i].empty())
					{
						nextFreeSlot = static_cast<int>(i);
						break;
					}
				}
			}
			else if (m_ChannelFilter == 1) // Trace 48..63
			{
				for (uint8_t i = Asset::CollisionChannelConfig::kTraceChannelStart;
					i < Asset::CollisionChannelConfig::kMaxChannels; ++i)
				{
					if (m_Physics.Channels.ChannelNames[i].empty())
					{
						nextFreeSlot = static_cast<int>(i);
						break;
					}
				}
			}
			else // All 0..63
			{
				for (uint8_t i = 0; i < Asset::CollisionChannelConfig::kMaxChannels; ++i)
				{
					if (m_Physics.Channels.ChannelNames[i].empty())
					{
						nextFreeSlot = static_cast<int>(i);
						break;
					}
				}
			}

			if (nextFreeSlot != -1)
			{
				if (Wui::Button(ctx, Wui::HashId("settings.collision.ch.add"),
					{ listRect.X + listRect.W - 68.0f, listRect.Y, 68.0f, 20.0f },
					Wui::Tr("settings.collision.add", "+ Add Slot"), theme))
				{
					const uint8_t slot = static_cast<uint8_t>(nextFreeSlot);
					const bool isSlotTrace = (slot >= Asset::CollisionChannelConfig::kTraceChannelStart);
					const std::string defName = (isSlotTrace ? "Trace_" : "Object_") + std::to_string(slot);
					m_Physics.Channels.SetChannelName(slot, defName);
					m_Physics.Channels.SetChannelType(slot,
						isSlotTrace ? Asset::CollisionChannelType::Trace : Asset::CollisionChannelType::Object);
					m_Physics.Channels.Matrix[slot].SetAll(Asset::CollisionResponse::Block);
					for (size_t k = 0; k < Asset::CollisionChannelConfig::kMaxChannels; ++k)
						m_Physics.Channels.Matrix[k].SetResponse(slot, Asset::CollisionResponse::Block);

					PhysicsSettings::Set(m_Physics);
					m_PendingSave = true;
					m_LastChangeSeconds = SettingsNowSeconds();
					m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
					m_StatusIsError = false;
				}
				Wui::Tooltip(ctx, { listRect.X + listRect.W - 68.0f, listRect.Y, 68.0f, 20.0f },
					Wui::Tr("settings.collision.add.tooltip", "Claim next available channel slot in this category"));
			}

			const float listBodyTop = listRect.Y + 24.0f;
			const float listBodyHeight = std::max(20.0f, listRect.H - 24.0f);
			const Wui::WuiRect listViewport { listRect.X, listBodyTop, listRect.W, listBodyHeight };

			std::vector<uint8_t> channelsToDisplay;
			if (m_ShowAllChannels)
			{
				if (m_ChannelFilter == 0)
				{
					channelsToDisplay.reserve(Asset::CollisionChannelConfig::kTraceChannelStart);
					for (uint8_t i = 0; i < Asset::CollisionChannelConfig::kTraceChannelStart; ++i)
						channelsToDisplay.push_back(i);
				}
				else if (m_ChannelFilter == 1)
				{
					channelsToDisplay.reserve(Asset::CollisionChannelConfig::kMaxChannels - Asset::CollisionChannelConfig::kTraceChannelStart);
					for (uint8_t i = Asset::CollisionChannelConfig::kTraceChannelStart;
						i < Asset::CollisionChannelConfig::kMaxChannels; ++i)
					{
						channelsToDisplay.push_back(i);
					}
				}
				else
				{
					channelsToDisplay.reserve(Asset::CollisionChannelConfig::kMaxChannels);
					for (uint8_t i = 0; i < Asset::CollisionChannelConfig::kMaxChannels; ++i)
						channelsToDisplay.push_back(i);
				}
			}
			else
			{
				if (m_ChannelFilter == 0)
					channelsToDisplay = activeObjectChannels;
				else if (m_ChannelFilter == 1)
					channelsToDisplay = activeTraceChannels;
				else
					channelsToDisplay = activeAllChannels;

				if (nextFreeSlot != -1)
					channelsToDisplay.push_back(static_cast<uint8_t>(nextFreeSlot));
			}

			const float totalListContentH = static_cast<float>(channelsToDisplay.size()) * 24.0f;
			Wui::BeginScrollArea(ctx, listViewport, totalListContentH, m_ChannelsScrollY, theme,
				Wui::HashId("settings.collision.ch.scroll"));

			for (size_t itemIdx = 0; itemIdx < channelsToDisplay.size(); ++itemIdx)
			{
				const uint8_t chIdx = channelsToDisplay[itemIdx];
				const float rowY = listViewport.Y + static_cast<float>(itemIdx) * 24.0f - m_ChannelsScrollY;
				if (rowY + 24.0f < listViewport.Y || rowY > listViewport.Y + listViewport.H)
					continue;

				const bool isActive = !m_Physics.Channels.ChannelNames[chIdx].empty();
				const bool isTraceSlot = (m_Physics.Channels.GetChannelType(chIdx) == Asset::CollisionChannelType::Trace ||
					chIdx >= Asset::CollisionChannelConfig::kTraceChannelStart);

				// 1. 槽位编号徽章
				char badge[16] = {};
				std::snprintf(badge, sizeof(badge), "#%u", static_cast<unsigned>(chIdx));
				Wui::Label(ctx, { listViewport.X + 2.0f, rowY + 3.0f }, badge,
					isActive ? theme.Text : theme.TextDisabled, 11.0f);

				// 2. 通道类型切换按钮 [Obj] / [Trc]
				const Wui::WuiRect typeRect { listViewport.X + 28.0f, rowY + 2.0f, 32.0f, 20.0f };
				const Wui::WuiId typeId = Wui::HashId(("settings.collision.ch.type." + std::to_string(chIdx)).c_str());
				Wui::WuiButtonStyle typeStyle;
				typeStyle.FontSize = 10.0f;
				typeStyle.PaddingX = 4.0f;
				for (size_t s = 0; s < Wui::WuiButtonStyle::StateCount; ++s)
				{
					typeStyle.Colors[s].Text = isTraceSlot ? theme.Warning : theme.Accent;
					if (s == static_cast<size_t>(Wui::WuiButtonStyle::State::Normal))
						typeStyle.Colors[s].Bg = theme.ContentBg;
				}
				if (Wui::Button(ctx, typeId, typeRect, isTraceSlot ? "Trc" : "Obj", theme, &typeStyle))
				{
					m_Physics.Channels.SetChannelType(chIdx,
						isTraceSlot ? Asset::CollisionChannelType::Object : Asset::CollisionChannelType::Trace);
					PhysicsSettings::Set(m_Physics);
					m_PendingSave = true;
					m_LastChangeSeconds = SettingsNowSeconds();
					m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
					m_StatusIsError = false;
				}
				Wui::Tooltip(ctx, typeRect, isTraceSlot
					? ("Channel #" + std::to_string(chIdx) + " Type: Trace (空间射线/追踪检测). Click to switch to Object.")
					: ("Channel #" + std::to_string(chIdx) + " Type: Object (实体碰撞体/刚体形状). Click to switch to Trace."));

				// 3. 通道名称编辑 / 认领
				const bool canClear = isActive && chIdx > 0;
				const float fieldX = listViewport.X + 64.0f;
				const float fieldW = listViewport.W - 66.0f - (canClear ? 22.0f : 0.0f);
				const Wui::WuiRect fieldRect { fieldX, rowY + 2.0f, fieldW, 20.0f };

				if (isActive)
				{
					const Wui::WuiId fieldId = Wui::HashId(("settings.collision.ch." + std::to_string(chIdx)).c_str());
					std::string& buf = ctx.Persist<std::string>(
						Wui::HashId(("settings.collision.ch." + std::to_string(chIdx) + ".buf").c_str()),
						m_Physics.Channels.ChannelNames[chIdx]);

					if (ctx.Focus() != fieldId)
						buf = m_Physics.Channels.ChannelNames[chIdx];

					if (Wui::TextField(ctx, fieldId, fieldRect, buf, theme) && buf != m_Physics.Channels.ChannelNames[chIdx])
					{
						m_Physics.Channels.SetChannelName(chIdx, buf);
						PhysicsSettings::Set(m_Physics);
						m_PendingSave = true;
						m_LastChangeSeconds = SettingsNowSeconds();
						m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
						m_StatusIsError = false;
					}

					if (canClear)
					{
						const Wui::WuiRect clearRect { listViewport.X + listViewport.W - 20.0f, rowY + 2.0f, 18.0f, 20.0f };
						if (Wui::Button(ctx, Wui::HashId(("settings.collision.ch.del." + std::to_string(chIdx)).c_str()),
							clearRect, "x", theme))
						{
							m_Physics.Channels.ChannelNames[chIdx].clear();
							buf.clear();
							PhysicsSettings::Set(m_Physics);
							m_PendingSave = true;
							m_LastChangeSeconds = SettingsNowSeconds();
							m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
							m_StatusIsError = false;
						}
						Wui::Tooltip(ctx, clearRect, Wui::Tr("settings.collision.clear_channel", "Deactivate channel"));
					}
				}
				else
				{
					// 未认领的空闲槽位
					if (Wui::Button(ctx, Wui::HashId(("settings.collision.ch.claim." + std::to_string(chIdx)).c_str()),
						fieldRect, Wui::Tr("settings.collision.claim", "+ Claim Slot"), theme))
					{
						const bool isDefaultTrace = (chIdx >= Asset::CollisionChannelConfig::kTraceChannelStart);
						m_Physics.Channels.SetChannelName(chIdx, (isDefaultTrace ? "Trace_" : "Object_") + std::to_string(chIdx));
						m_Physics.Channels.SetChannelType(chIdx,
							isDefaultTrace ? Asset::CollisionChannelType::Trace : Asset::CollisionChannelType::Object);
						m_Physics.Channels.Matrix[chIdx].SetAll(Asset::CollisionResponse::Block);
						for (size_t k = 0; k < Asset::CollisionChannelConfig::kMaxChannels; ++k)
							m_Physics.Channels.Matrix[k].SetResponse(chIdx, Asset::CollisionResponse::Block);

						PhysicsSettings::Set(m_Physics);
						m_PendingSave = true;
						m_LastChangeSeconds = SettingsNowSeconds();
						m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
						m_StatusIsError = false;
					}
					Wui::Tooltip(ctx, fieldRect, "Activate and claim channel slot #" + std::to_string(chIdx));
				}
			}
			Wui::EndScrollArea(ctx);
		}

		// --- 三态彩色网格矩阵 (3-State Colored Grid Cells) ---
		{
			std::vector<uint8_t> matrixRows;
			std::vector<uint8_t> matrixCols;
			bool isSymmetric = false;

			if (m_ChannelFilter == 0) // Object 交互矩阵
			{
				matrixRows = activeObjectChannels;
				matrixCols = activeObjectChannels;
				isSymmetric = true;
			}
			else if (m_ChannelFilter == 1) // Trace 检测矩阵: Trace 通道为行，Object 通道为列
			{
				matrixRows = activeTraceChannels;
				matrixCols = activeObjectChannels.empty() ? activeTraceChannels : activeObjectChannels;
				isSymmetric = false;
			}
			else // All 全量 64 通道对称矩阵
			{
				matrixRows = activeAllChannels;
				matrixCols = activeAllChannels;
				isSymmetric = true;
			}

			const size_t numRows = matrixRows.size();
			const size_t numCols = matrixCols.size();
			constexpr float kRowHeaderW = 104.0f;
			constexpr float kColW = 40.0f;
			constexpr float kHeaderH = 24.0f;
			constexpr float kRowH = 24.0f;

			// 固定列标题栏
			const Wui::WuiRect colHeaderBar { matrixRect.X, matrixRect.Y, matrixRect.W, kHeaderH };
			const std::string cornerLabel = (m_ChannelFilter == 1) ? "Trace \\ Obj" : "Channel";
			Wui::Label(ctx, { matrixRect.X + 4.0f, matrixRect.Y + 4.0f },
				cornerLabel, theme.TextMuted, 11.0f);

			for (size_t c = 0; c < numCols; ++c)
			{
				const uint8_t chB = matrixCols[c];
				const float colX = matrixRect.X + kRowHeaderW + static_cast<float>(c) * kColW;
				const Wui::WuiRect colHeaderRect { colX, matrixRect.Y, kColW, kHeaderH };

				std::string colName = m_Physics.Channels.ChannelNames[chB];
				std::string shortName = colName.size() > 4 ? colName.substr(0, 3) + "." : colName;
				std::string colLabel = "#" + std::to_string(chB) + " " + shortName;

				Wui::Label(ctx, { colX + 2.0f, matrixRect.Y + 4.0f }, colLabel, theme.TextMuted, 11.0f);
				const bool isTraceB = (m_Physics.Channels.GetChannelType(chB) == Asset::CollisionChannelType::Trace);
				Wui::Tooltip(ctx, colHeaderRect,
					"Column #" + std::to_string(chB) + ": " + colName + (isTraceB ? " (Trace)" : " (Object)"));
			}

			// 可滚动数据行区域
			const float rowsTop = matrixRect.Y + kHeaderH + 2.0f;
			const float rowsH = std::max(20.0f, matrixRect.H - kHeaderH - 2.0f);
			const Wui::WuiRect rowsViewport { matrixRect.X, rowsTop, matrixRect.W, rowsH };

			const float totalRowsH = static_cast<float>(numRows) * kRowH;
			Wui::BeginScrollArea(ctx, rowsViewport, totalRowsH, m_CollisionScrollY, theme,
				Wui::HashId("settings.collision.matrix.scroll"));

			for (size_t r = 0; r < numRows; ++r)
			{
				const float rowY = rowsViewport.Y + static_cast<float>(r) * kRowH - m_CollisionScrollY;
				if (rowY + kRowH < rowsViewport.Y || rowY > rowsViewport.Y + rowsViewport.H)
					continue;

				const uint8_t chA = matrixRows[r];
				const Wui::WuiRect rowHeaderRect { rowsViewport.X, rowY, kRowHeaderW, kRowH };

				std::string rName = m_Physics.Channels.ChannelNames[chA];
				std::string shortRName = rName.size() > 9 ? rName.substr(0, 8) + "." : rName;
				std::string rowLabel = "#" + std::to_string(chA) + " " + shortRName;

				const bool isTraceA = (m_Physics.Channels.GetChannelType(chA) == Asset::CollisionChannelType::Trace);
				Wui::Label(ctx, { rowsViewport.X + 4.0f, rowY + 4.0f }, rowLabel,
					isTraceA ? theme.Warning : theme.Text, 11.0f);
				Wui::Tooltip(ctx, rowHeaderRect,
					"Row #" + std::to_string(chA) + ": " + rName + (isTraceA ? " (Trace)" : " (Object)"));

				for (size_t c = 0; c < numCols; ++c)
				{
					const uint8_t chB = matrixCols[c];
					const float cellX = rowsViewport.X + kRowHeaderW + static_cast<float>(c) * kColW;
					const Wui::WuiRect cellRect { cellX, rowY, kColW - 2.0f, kRowH - 2.0f };

					if (isSymmetric && m_CollisionTriangleOnly && c > r)
					{
						Wui::Label(ctx, { cellX + (kColW - 8.0f) * 0.5f, rowY + 4.0f },
							"-", theme.TextDisabled, 11.0f);
						continue;
					}

					const Asset::CollisionResponse current = m_Physics.Channels.GetCollisionResponse(chA, chB);
					Asset::CollisionResponse nextResp = Asset::CollisionResponse::Block;
					if (current == Asset::CollisionResponse::Block)
						nextResp = Asset::CollisionResponse::Overlap;
					else if (current == Asset::CollisionResponse::Overlap)
						nextResp = Asset::CollisionResponse::Ignore;
					else
						nextResp = Asset::CollisionResponse::Block;

					std::string cellLabel = "[-]";
					std::string tipKind = "Ignore (完全穿透忽略)";
					Wui::WuiColor textColor = theme.TextDisabled;

					if (current == Asset::CollisionResponse::Block)
					{
						cellLabel = "[B]";
						tipKind = "Block (物理硬阻挡)";
						textColor = theme.Success;
					}
					else if (current == Asset::CollisionResponse::Overlap)
					{
						cellLabel = "[O]";
						tipKind = "Overlap (穿透触发事件)";
						textColor = theme.Warning;
					}

					Wui::WuiButtonStyle cellStyle;
					cellStyle.FontSize = 11.0f;
					cellStyle.PaddingX = (cellRect.W - 18.0f) * 0.5f;
					for (size_t s = 0; s < Wui::WuiButtonStyle::StateCount; ++s)
					{
						cellStyle.Colors[s].Text = textColor;
						if (s == static_cast<size_t>(Wui::WuiButtonStyle::State::Normal))
							cellStyle.Colors[s].Bg = theme.ContentBg;
					}

					const Wui::WuiId cellId = Wui::HashId(
						("settings.collision.cell." + std::to_string(chA) + "." + std::to_string(chB)).c_str());

					if (Wui::Button(ctx, cellId, cellRect, cellLabel, theme, &cellStyle))
					{
						m_Physics.Channels.SetCollisionResponse(chA, chB, nextResp);
						PhysicsSettings::Set(m_Physics);
						m_PendingSave = true;
						m_LastChangeSeconds = SettingsNowSeconds();
						m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
						m_StatusIsError = false;
					}

					std::string tipStr = tipKind + "\n#" + std::to_string(chA) + " (" + rName +
						") ↔ #" + std::to_string(chB) + " (" + m_Physics.Channels.ChannelNames[chB] +
						")\nClick to cycle: Block -> Overlap -> Ignore";
					Wui::Tooltip(ctx, cellRect, tipStr);
				}
			}

			Wui::EndScrollArea(ctx);
		}
	}

	void SettingsPanel::RenderCollisionProfiles(Wui::WuiContext& ctx, const Wui::WuiRect& page,
		const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		EnsureDefaultProfiles();

		// 1. 顶部工具栏
		float curX = page.X;
		const float toolBarY = page.Y;
		if (Wui::Button(ctx, Wui::HashId("settings.profiles.back"),
			{ curX, toolBarY, 116.0f, 24.0f },
			Wui::Tr("settings.profiles.back", "< Back to Matrix"), theme))
		{
			m_CollisionSubTab = 0;
		}
		Wui::Tooltip(ctx, { curX, toolBarY, 116.0f, 24.0f }, "Return to Collision Matrix grid view");
		curX += 120.0f;

		if (Wui::Button(ctx, Wui::HashId("settings.profiles.reset"),
			{ curX, toolBarY, 156.0f, 24.0f },
			Wui::Tr("settings.profiles.reset", "Reset Default Profiles"), theme))
		{
			m_Physics.Channels.Profiles = Asset::CollisionChannelConfig::Default().Profiles;
			EnsureDefaultProfiles();
			PhysicsSettings::Set(m_Physics);
			m_PendingSave = true;
			m_LastChangeSeconds = SettingsNowSeconds();
			m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
			m_StatusIsError = false;
		}
		Wui::Tooltip(ctx, { curX, toolBarY, 156.0f, 24.0f },
			"Restore preset collision profiles to default response configurations");
		curX += 164.0f;

		Wui::Label(ctx, { curX + 4.0f, toolBarY + 5.0f },
			"Collision Profiles assign object archetypes preset responses across all 64 channels",
			theme.TextMuted, 12.0f);

		// 2. 左右分割布局: 左侧 Profiles 列表, 右侧所选 Profile 详情与通道响应
		const float contentTop = page.Y + 28.0f;
		const float contentH = std::max(60.0f, page.H - 28.0f);
		const bool narrow = page.W < 600.0f;
		const float listW = narrow ? page.W : 220.0f;
		const float listH = narrow ? 140.0f : contentH;
		const Wui::WuiRect profListRect { page.X, contentTop, listW, listH };

		const float detailX = narrow ? page.X : page.X + listW + 12.0f;
		const float detailY = narrow ? contentTop + listH + 8.0f : contentTop;
		const float detailW = narrow ? page.W : page.W - listW - 12.0f;
		const float detailH = narrow ? std::max(60.0f, contentH - listH - 8.0f) : contentH;
		const Wui::WuiRect detailRect { detailX, detailY, detailW, detailH };

		// --- 左侧: 预设列表 (Profiles List) ---
		{
			char profHeaderBuf[64] = {};
			std::snprintf(profHeaderBuf, sizeof(profHeaderBuf), "Profiles (%d)",
				static_cast<int>(m_Physics.Channels.Profiles.size()));
			Wui::Label(ctx, { profListRect.X + 2.0f, profListRect.Y + 2.0f },
				profHeaderBuf, theme.Text, 13.0f);

			const float listBodyTop = profListRect.Y + 24.0f;
			const float listBodyH = std::max(20.0f, profListRect.H - 24.0f);
			const Wui::WuiRect listViewport { profListRect.X, listBodyTop, profListRect.W, listBodyH };

			const float totalListH = static_cast<float>(m_Physics.Channels.Profiles.size()) * 28.0f;
			Wui::BeginScrollArea(ctx, listViewport, totalListH, m_ProfileListScrollY, theme,
				Wui::HashId("settings.collision.profiles.scroll"));

			for (size_t i = 0; i < m_Physics.Channels.Profiles.size(); ++i)
			{
				const float itemY = listViewport.Y + static_cast<float>(i) * 28.0f - m_ProfileListScrollY;
				if (itemY + 28.0f < listViewport.Y || itemY > listViewport.Y + listViewport.H)
					continue;

				const auto& prof = m_Physics.Channels.Profiles[i];
				const bool isSelected = (m_SelectedProfile == static_cast<int>(i));
				const Wui::WuiRect itemRect { listViewport.X, itemY + 2.0f, listViewport.W - 6.0f, 24.0f };
				const Wui::WuiId itemId = Wui::HashId(("settings.profile.item." + std::to_string(i)).c_str());

				std::string itemLabel = prof.Name;
				if (prof.ObjectType < Asset::CollisionChannelConfig::kMaxChannels &&
					!m_Physics.Channels.ChannelNames[prof.ObjectType].empty())
				{
					itemLabel += " [" + m_Physics.Channels.ChannelNames[prof.ObjectType] + "]";
				}

				if (Wui::ButtonEx(ctx, itemId, itemRect, itemLabel, theme, true, isSelected,
					"Select profile: " + prof.Name))
				{
					m_SelectedProfile = static_cast<int>(i);
				}
			}

			Wui::EndScrollArea(ctx);
		}

		// --- 右侧: 所选 Profile 详情与通道响应 ---
		if (m_SelectedProfile < 0) m_SelectedProfile = 0;
		if (m_SelectedProfile >= static_cast<int>(m_Physics.Channels.Profiles.size()))
			m_SelectedProfile = static_cast<int>(m_Physics.Channels.Profiles.size()) - 1;

		if (m_SelectedProfile >= 0 && m_SelectedProfile < static_cast<int>(m_Physics.Channels.Profiles.size()))
		{
			auto& profile = m_Physics.Channels.Profiles[m_SelectedProfile];

			// 详情头部第 1 行: Profile 名称与绑定的 Object Channel
			float cardY = detailRect.Y;
			Wui::Label(ctx, { detailRect.X + 2.0f, cardY + 2.0f },
				"Profile: " + profile.Name, theme.Text, 14.0f);

			std::string objTypeName = (profile.ObjectType < Asset::CollisionChannelConfig::kMaxChannels &&
				!m_Physics.Channels.ChannelNames[profile.ObjectType].empty())
				? m_Physics.Channels.ChannelNames[profile.ObjectType]
				: "Channel #" + std::to_string(profile.ObjectType);

			char objTypeBuf[128] = {};
			std::snprintf(objTypeBuf, sizeof(objTypeBuf), "Object Type: #%u (%s)",
				static_cast<unsigned>(profile.ObjectType), objTypeName.c_str());
			Wui::Label(ctx, { detailRect.X + 180.0f, cardY + 2.0f }, objTypeBuf, theme.Accent, 12.0f);

			// 切换绑定的 Object Channel 槽位 (< 和 > 按钮)
			if (Wui::Button(ctx, Wui::HashId("settings.profile.prev_obj"),
				{ detailRect.X + 380.0f, cardY, 24.0f, 20.0f }, "<", theme))
			{
				for (int ch = static_cast<int>(profile.ObjectType) - 1; ch >= 0; --ch)
				{
					if (!m_Physics.Channels.ChannelNames[ch].empty() &&
						m_Physics.Channels.GetChannelType(static_cast<uint8_t>(ch)) == Asset::CollisionChannelType::Object)
					{
						profile.ObjectType = static_cast<uint8_t>(ch);
						PhysicsSettings::Set(m_Physics);
						m_PendingSave = true;
						m_LastChangeSeconds = SettingsNowSeconds();
						m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
						m_StatusIsError = false;
						break;
					}
				}
			}
			Wui::Tooltip(ctx, { detailRect.X + 380.0f, cardY, 24.0f, 20.0f }, "Switch to previous active Object channel");

			if (Wui::Button(ctx, Wui::HashId("settings.profile.next_obj"),
				{ detailRect.X + 408.0f, cardY, 24.0f, 20.0f }, ">", theme))
			{
				for (size_t ch = profile.ObjectType + 1; ch < Asset::CollisionChannelConfig::kTraceChannelStart; ++ch)
				{
					if (!m_Physics.Channels.ChannelNames[ch].empty() &&
						m_Physics.Channels.GetChannelType(static_cast<uint8_t>(ch)) == Asset::CollisionChannelType::Object)
					{
						profile.ObjectType = static_cast<uint8_t>(ch);
						PhysicsSettings::Set(m_Physics);
						m_PendingSave = true;
						m_LastChangeSeconds = SettingsNowSeconds();
						m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
						m_StatusIsError = false;
						break;
					}
				}
			}
			Wui::Tooltip(ctx, { detailRect.X + 408.0f, cardY, 24.0f, 20.0f }, "Switch to next active Object channel");

			// 详情头部第 2 行: 批量设置按钮与通道过滤
			cardY += 28.0f;
			float batchX = detailRect.X;

			if (Wui::Button(ctx, Wui::HashId("settings.profile.all_block"),
				{ batchX, cardY, 84.0f, 22.0f }, Wui::Tr("settings.profile.all_block", "All Block"), theme))
			{
				profile.Responses.SetAll(Asset::CollisionResponse::Block);
				PhysicsSettings::Set(m_Physics);
				m_PendingSave = true;
				m_LastChangeSeconds = SettingsNowSeconds();
				m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
				m_StatusIsError = false;
			}
			Wui::Tooltip(ctx, { batchX, cardY, 84.0f, 22.0f }, "Set all channel responses in this profile to Block");
			batchX += 88.0f;

			if (Wui::Button(ctx, Wui::HashId("settings.profile.all_overlap"),
				{ batchX, cardY, 92.0f, 22.0f }, Wui::Tr("settings.profile.all_overlap", "All Overlap"), theme))
			{
				profile.Responses.SetAll(Asset::CollisionResponse::Overlap);
				PhysicsSettings::Set(m_Physics);
				m_PendingSave = true;
				m_LastChangeSeconds = SettingsNowSeconds();
				m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
				m_StatusIsError = false;
			}
			Wui::Tooltip(ctx, { batchX, cardY, 92.0f, 22.0f }, "Set all channel responses in this profile to Overlap");
			batchX += 96.0f;

			if (Wui::Button(ctx, Wui::HashId("settings.profile.all_ignore"),
				{ batchX, cardY, 84.0f, 22.0f }, Wui::Tr("settings.profile.all_ignore", "All Ignore"), theme))
			{
				profile.Responses.SetAll(Asset::CollisionResponse::Ignore);
				PhysicsSettings::Set(m_Physics);
				m_PendingSave = true;
				m_LastChangeSeconds = SettingsNowSeconds();
				m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
				m_StatusIsError = false;
			}
			Wui::Tooltip(ctx, { batchX, cardY, 84.0f, 22.0f }, "Set all channel responses in this profile to Ignore");
			batchX += 96.0f;

			// 预设通道表格过滤按钮
			const bool profFilterAll = (m_ProfileChannelFilter == 0);
			const bool profFilterObj = (m_ProfileChannelFilter == 1);
			const bool profFilterTrace = (m_ProfileChannelFilter == 2);

			if (Wui::ButtonEx(ctx, Wui::HashId("settings.profile.filter.all"),
				{ batchX, cardY, 80.0f, 22.0f }, "All", theme, true, profFilterAll, "Show all active channels"))
			{
				m_ProfileChannelFilter = 0;
			}
			batchX += 84.0f;

			if (Wui::ButtonEx(ctx, Wui::HashId("settings.profile.filter.obj"),
				{ batchX, cardY, 84.0f, 22.0f }, "Object", theme, true, profFilterObj, "Filter to Object channels"))
			{
				m_ProfileChannelFilter = 1;
			}
			batchX += 88.0f;

			if (Wui::ButtonEx(ctx, Wui::HashId("settings.profile.filter.trace"),
				{ batchX, cardY, 80.0f, 22.0f }, "Trace", theme, true, profFilterTrace, "Filter to Trace channels"))
			{
				m_ProfileChannelFilter = 2;
			}

			// 通道响应表格 (Channel Responses Table)
			const float tableTop = cardY + 28.0f;
			const float tableH = std::max(40.0f, detailRect.H - 58.0f);

			// 表头
			Wui::Label(ctx, { detailRect.X + 4.0f, tableTop + 2.0f }, "Target Channel", theme.TextMuted, 11.0f);
			Wui::Label(ctx, { detailRect.X + 160.0f, tableTop + 2.0f }, "Type", theme.TextMuted, 11.0f);
			Wui::Label(ctx, { detailRect.X + 224.0f, tableTop + 2.0f }, "Response Selection", theme.TextMuted, 11.0f);

			std::vector<uint8_t> channelsToDisplay;
			channelsToDisplay.reserve(Asset::CollisionChannelConfig::kMaxChannels);
			for (uint8_t ch = 0; ch < static_cast<uint8_t>(Asset::CollisionChannelConfig::kMaxChannels); ++ch)
			{
				if (m_Physics.Channels.ChannelNames[ch].empty())
					continue;

				const bool isChTrace = (m_Physics.Channels.GetChannelType(ch) == Asset::CollisionChannelType::Trace ||
					ch >= Asset::CollisionChannelConfig::kTraceChannelStart);

				if (m_ProfileChannelFilter == 1 && isChTrace)
					continue;
				if (m_ProfileChannelFilter == 2 && !isChTrace)
					continue;

				channelsToDisplay.push_back(ch);
			}

			const float rowsViewportTop = tableTop + 20.0f;
			const float rowsViewportH = std::max(20.0f, tableH - 20.0f);
			const Wui::WuiRect tableArea { detailRect.X, rowsViewportTop, detailRect.W, rowsViewportH };

			const float totalTableH = static_cast<float>(channelsToDisplay.size()) * 26.0f;
			Wui::BeginScrollArea(ctx, tableArea, totalTableH, m_ProfileScrollY, theme,
				Wui::HashId("settings.profile.responses.scroll"));

			for (size_t rowIdx = 0; rowIdx < channelsToDisplay.size(); ++rowIdx)
			{
				const uint8_t ch = channelsToDisplay[rowIdx];
				const float rowY = tableArea.Y + static_cast<float>(rowIdx) * 26.0f - m_ProfileScrollY;
				if (rowY + 26.0f < tableArea.Y || rowY > tableArea.Y + tableArea.H)
					continue;

				const std::string& chName = m_Physics.Channels.ChannelNames[ch];
				const bool isChTrace = (m_Physics.Channels.GetChannelType(ch) == Asset::CollisionChannelType::Trace ||
					ch >= Asset::CollisionChannelConfig::kTraceChannelStart);
				const Asset::CollisionResponse curResp = profile.Responses.GetResponse(ch);

				// 1. 通道标签
				std::string chLabel = "#" + std::to_string(ch) + " " + chName;
				Wui::Label(ctx, { tableArea.X + 4.0f, rowY + 5.0f }, chLabel, theme.Text, 12.0f);

				// 2. 类型标识
				Wui::Label(ctx, { tableArea.X + 160.0f, rowY + 5.0f },
					isChTrace ? "Trace" : "Object", isChTrace ? theme.Warning : theme.Accent, 11.0f);

				// 3. 响应三选一按钮: [B] Block / [O] Overlap / [-] Ignore
				const float btnBaseX = tableArea.X + 224.0f;
				const bool isBlock = (curResp == Asset::CollisionResponse::Block);
				const bool isOverlap = (curResp == Asset::CollisionResponse::Overlap);
				const bool isIgnore = (curResp == Asset::CollisionResponse::Ignore);

				// Block 按钮
				const Wui::WuiId blockId = Wui::HashId(("settings.profile.resp.b." + std::to_string(m_SelectedProfile) + "." + std::to_string(ch)).c_str());
				Wui::WuiButtonStyle blockStyle;
				blockStyle.FontSize = 11.0f;
				blockStyle.PaddingX = 8.0f;
				for (size_t s = 0; s < Wui::WuiButtonStyle::StateCount; ++s)
				{
					blockStyle.Colors[s].Text = isBlock ? theme.Success : theme.TextDisabled;
					if (s == static_cast<size_t>(Wui::WuiButtonStyle::State::Normal))
						blockStyle.Colors[s].Bg = isBlock ? theme.ActiveBg : theme.ContentBg;
				}
				if (Wui::Button(ctx, blockId, { btnBaseX, rowY + 2.0f, 56.0f, 22.0f }, "[B] Block", theme, &blockStyle))
				{
					profile.Responses.SetResponse(ch, Asset::CollisionResponse::Block);
					PhysicsSettings::Set(m_Physics);
					m_PendingSave = true;
					m_LastChangeSeconds = SettingsNowSeconds();
					m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
					m_StatusIsError = false;
				}
				Wui::Tooltip(ctx, { btnBaseX, rowY + 2.0f, 56.0f, 22.0f },
					"Block (物理硬阻挡) — Profile '" + profile.Name + "' ↔ Channel '" + chName + "'");

				// Overlap 按钮
				const Wui::WuiId overlapId = Wui::HashId(("settings.profile.resp.o." + std::to_string(m_SelectedProfile) + "." + std::to_string(ch)).c_str());
				Wui::WuiButtonStyle overlapStyle;
				overlapStyle.FontSize = 11.0f;
				overlapStyle.PaddingX = 6.0f;
				for (size_t s = 0; s < Wui::WuiButtonStyle::StateCount; ++s)
				{
					overlapStyle.Colors[s].Text = isOverlap ? theme.Warning : theme.TextDisabled;
					if (s == static_cast<size_t>(Wui::WuiButtonStyle::State::Normal))
						overlapStyle.Colors[s].Bg = isOverlap ? theme.ActiveBg : theme.ContentBg;
				}
				if (Wui::Button(ctx, overlapId, { btnBaseX + 60.0f, rowY + 2.0f, 68.0f, 22.0f }, "[O] Overlap", theme, &overlapStyle))
				{
					profile.Responses.SetResponse(ch, Asset::CollisionResponse::Overlap);
					PhysicsSettings::Set(m_Physics);
					m_PendingSave = true;
					m_LastChangeSeconds = SettingsNowSeconds();
					m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
					m_StatusIsError = false;
				}
				Wui::Tooltip(ctx, { btnBaseX + 60.0f, rowY + 2.0f, 68.0f, 22.0f },
					"Overlap (穿透触发事件) — Profile '" + profile.Name + "' ↔ Channel '" + chName + "'");

				// Ignore 按钮
				const Wui::WuiId ignoreId = Wui::HashId(("settings.profile.resp.i." + std::to_string(m_SelectedProfile) + "." + std::to_string(ch)).c_str());
				Wui::WuiButtonStyle ignoreStyle;
				ignoreStyle.FontSize = 11.0f;
				ignoreStyle.PaddingX = 8.0f;
				for (size_t s = 0; s < Wui::WuiButtonStyle::StateCount; ++s)
				{
					ignoreStyle.Colors[s].Text = isIgnore ? theme.Text : theme.TextDisabled;
					if (s == static_cast<size_t>(Wui::WuiButtonStyle::State::Normal))
						ignoreStyle.Colors[s].Bg = isIgnore ? theme.ActiveBg : theme.ContentBg;
				}
				if (Wui::Button(ctx, ignoreId, { btnBaseX + 132.0f, rowY + 2.0f, 56.0f, 22.0f }, "[-] Ignore", theme, &ignoreStyle))
				{
					profile.Responses.SetResponse(ch, Asset::CollisionResponse::Ignore);
					PhysicsSettings::Set(m_Physics);
					m_PendingSave = true;
					m_LastChangeSeconds = SettingsNowSeconds();
					m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
					m_StatusIsError = false;
				}
				Wui::Tooltip(ctx, { btnBaseX + 132.0f, rowY + 2.0f, 56.0f, 22.0f },
					"Ignore (完全穿透忽略) — Profile '" + profile.Name + "' ↔ Channel '" + chName + "'");
			}

			Wui::EndScrollArea(ctx);
		}
	}
	void SettingsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		m_Host = &host;
		if (!m_Initialized)
		{
			m_Edit = RenderSettings::Get();
			m_Physics = PhysicsSettings::Get();
			EnsureDefaultProfiles();
			// P4-UX11:启动项来自清单(以前只能手改 project.we.yaml)。找不到清单时显示当前后端,
			// 不让用户面对空白控件。
			m_Startup.Renderer = Renderer::GetBackendName();
			std::filesystem::path manifestPath;
			Asset::ProjectManifest manifest;
			std::string manifestError;
			if (Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath)
				&& Asset::ProjectManifest::Load(manifestPath, &manifest, &manifestError))
			{
				if (!manifest.Renderer.empty())
					m_Startup.Renderer = manifest.Renderer;
				m_Startup.StartScene = manifest.StartScene;
				m_Startup.ContentRoot = manifest.ContentRoot.generic_string();
				m_Packages = manifest.Packages;
				// P4-U4:导入默认值同源(清单是事实源);顺手写进进程级默认,让"打开面板"
				// 这一动作就把本会话的导入默认值对齐到清单(与启动期 LoadProjectDefaults 幂等)。
				m_Import = manifest.ImportDefaults;
				Asset::ModelImportSettings::SetProjectDefaults(m_Import);
				m_Streaming = manifest.Streaming;
			}
			else
			{
				WLD_CORE_WARN("项目设置面板:读取 project.we.yaml 失败({0})", manifestError);
			}
			m_SceneOptions = host.ListProjectScenes();
			m_StartupContentRootBuffer = m_Startup.ContentRoot;
			m_Initialized = true;
		}
		if (!m_Registered)
		{
			RegisterProjectSettings();
			m_Registered = true;
		}

		const float pad = 10.0f;
		// P4-U4:四个作用域里的三个在**本面板**按页签分家(Editor Preferences 有自己的面板):
		//   Project(渲染/物理/启动/发行包) | Import Defaults(project.we.yaml 的 imports:) |
		//   World(关卡 manifest:物理调试 + 场景重力)。
		// 为什么合成页签而不是三个面板:它们共享同一份"项目上下文",拆成三个面板会让
		// Window 菜单里出现三个名字相近的入口;页签是 Unity/Blender 的同类做法。
		const float tabHeight = 28.0f;
		{
			const std::vector<std::string> tabs {
				Wui::Tr("panel.settings.tab.project", "Project"),
				Wui::Tr("panel.settings.tab.imports", "Import Defaults"),
				Wui::Tr("panel.settings.tab.collision", "Collision Matrix"),
				Wui::Tr("panel.settings.tab.world", "World"),
			};
			const Wui::WuiRect tabRect { rect.X + pad, rect.Y + 4.0f, rect.W - pad * 2.0f, tabHeight };
			if (Wui::TabBar(ctx, Wui::HashId("settings.tabs"), tabRect, tabs, m_Tab, theme))
			{
				// 换页清搜索与滚动:否则会出现"页签变了、列表还是上一页的过滤结果"。
				m_Page.Search.clear();
				m_ImportPage.Search.clear();
				m_WorldPage.Search.clear();
			}
		}
		const float pageTop = rect.Y + 4.0f + tabHeight + 6.0f;
		const Wui::WuiRect page { rect.X + pad, pageTop, rect.W - pad * 2.0f,
			std::max(120.0f, rect.Y + rect.H - pageTop - 116.0f) };
		const std::vector<std::string> groups { "Rendering", "Physics", "Streaming", "Startup" };
		bool changed = false;
		if (m_Tab == 0)
		{
			changed = Editor::DrawSettingsPage(ctx, page, theme, Settings::SettingScope::Project, groups, m_Page);
		}
		else if (m_Tab == 1)
		{
			if (!m_ImportRegistered)
			{
				RegisterImportSettings();
				m_ImportRegistered = true;
			}
			if (Editor::DrawSettingsPage(ctx, page, theme, Settings::SettingScope::Import, { "Model" }, m_ImportPage))
			{
				m_ImportDirty = true;
				m_LastChangeSeconds = SettingsNowSeconds();
				m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
				m_StatusIsError = false;
			}
		}
		else if (m_Tab == 2)
		{
			RenderCollisionTab(ctx, page, theme, host);
		}
		else
		{
			if (!m_WorldRegistered)
			{
				RegisterWorldSettings();
				m_WorldRegistered = true;
			}
			if (Editor::DrawSettingsPage(ctx, page, theme, Settings::SettingScope::World, { "Physics" }, m_WorldPage))
			{
				// 场景级设置写进当前文档:标脏 + 立刻生效;落盘跟着"保存场景"走
				// (与项目清单的自动保存不同 —— 场景保存是用户的显式动作)。
				host.MarkDocumentDirty();
				m_Status = Wui::Tr("settings.world.status.applied",
					"Applied to the current scene — save the scene to keep it");
				m_StatusIsError = false;
			}
		}

		// 手写部分:发行包列表(project.we.yaml 的 `packages:`)。需要真正的"列表编辑"而不是一行文本,
		// 所以不进注册表(注册表是"一行一个值"的模型)。
		float y = page.Y + page.H + 6.0f;
		if (m_Tab == 0)
		{
			const Wui::LocalizedLabel header = Wui::TrLabel("settings.group.packages", "Packages (distribution)");
			Wui::LabelWithTerm(ctx, { rect.X + pad, y }, header.Text, header.Term, theme.TextMuted, 12.0f, theme);
			y += 20.0f;
			bool packageChanged = false;
			for (size_t i = 0; i < m_Packages.size(); ++i)
			{
				const Wui::WuiId rowId = Wui::HashId(("settings.packages." + std::to_string(i)).c_str());
				// 缓冲用派生 id:TextField 内部用控件 id 存 WuiEditState,同 id 存 string 会类型冲突。
				std::string& buffer = ctx.Persist<std::string>(
					Wui::HashId((std::string("settings.packages.") + std::to_string(i) + ".buffer").c_str()),
					m_Packages[i]);
				const Wui::WuiRect field { rect.X + pad, y, 240.0f, 20.0f };
				if (Wui::TextField(ctx, rowId, field, buffer, theme) && buffer != m_Packages[i])
				{
					m_Packages[i] = buffer;
					packageChanged = true;
				}
				if (Wui::Button(ctx, Wui::HashId((std::string("settings.packages.remove.") + std::to_string(i)).c_str()),
					{ rect.X + pad + 246.0f, y, 22.0f, 20.0f }, "x", theme))
				{
					m_Packages.erase(m_Packages.begin() + static_cast<std::ptrdiff_t>(i));
					packageChanged = true;
					break;
				}
				Wui::Tooltip(ctx, { rect.X + pad, y, 268.0f, 20.0f }, Wui::Tr("settings.packages.row.tooltip",
					"Package path\nRelative to the distribution root (e.g. packages/Base.wpak). Enter to apply, x to remove."));
				y += 24.0f;
			}
			if (Wui::Button(ctx, Wui::HashId("settings.packages.add"), { rect.X + pad, y, 96.0f, 20.0f },
				Wui::Tr("settings.packages.add", "+ Add Package"), theme))
			{
				m_Packages.emplace_back("packages/");
				packageChanged = true;
			}
			if (m_Packages.empty())
				Wui::Label(ctx, { rect.X + pad + 104.0f, y + 4.0f },
					Wui::Tr("settings.packages.empty", "No packages — the Runtime loads only the loose content root."),
					theme.TextMuted, 12.0f);
			if (packageChanged)
			{
				std::string message;
				if (host.SaveProjectPackages(m_Packages, &message))
				{
					m_Status = Wui::Tr("settings.status.saved", "Saved automatically");
					m_StatusIsError = false;
				}
				else
				{
					m_Status = message;
					m_StatusIsError = true;
				}
			}
			y += 26.0f;
		}

		if (changed)
		{
			// 渲染/物理是**暂存后统一应用 + 防抖落盘**;启动项在上面已经即时写清单了。
			if (m_Edit.MaxDirectionalLights + m_Edit.MaxPointLights > Renderer3D::MaxLights)
				m_Edit.MaxPointLights = Renderer3D::MaxLights - m_Edit.MaxDirectionalLights;
			RenderSettings::Set(m_Edit);
			PhysicsSettings::Set(m_Physics);
			Renderer::SetVsync(m_Edit.Vsync);
			m_PendingSave = true;
			m_LastChangeSeconds = SettingsNowSeconds();
			m_Status = Wui::Tr("settings.status.applied", "Applied — saving…");
			m_StatusIsError = false;
		}

		if (m_PendingSave && SettingsNowSeconds() - m_LastChangeSeconds >= 0.4)
		{
			std::string message;
			if (host.SaveProjectRenderSettings(m_Edit, &message) &&
				host.SaveProjectPhysicsSettings(m_Physics, &message) &&
				host.SaveProjectStreamingSettings(m_Streaming, &message))
			{
				m_Status = Wui::Tr("settings.status.saved", "Saved automatically");
				m_StatusIsError = false;
			}
			else
			{
				m_Status = message.empty() ? Wui::Tr("settings.status.save_failed", "Save failed") : message;
				m_StatusIsError = true;
			}
			m_PendingSave = false;
		}

		// P4-U4:导入默认值走同一套防抖(拖滑杆/连点勾选时不每帧写盘)。
		if (m_ImportDirty && SettingsNowSeconds() - m_LastChangeSeconds >= 0.4)
		{
			std::string message;
			if (host.SaveProjectImportDefaults(m_Import, &message))
			{
				m_Status = Wui::Tr("settings.status.saved", "Saved automatically");
				m_StatusIsError = false;
			}
			else
			{
				m_Status = message.empty() ? Wui::Tr("settings.status.save_failed", "Save failed") : message;
				m_StatusIsError = true;
			}
			m_ImportDirty = false;
		}

		// 渲染后端"改了但还没重启":给一个显式入口(不替用户重启)。
		if (m_Startup.Renderer != Renderer::GetBackendName())
		{
			if (Wui::Button(ctx, Wui::HashId("settings.project.renderer.restart"),
				{ rect.X + pad, y, 220.0f, 22.0f },
				Wui::Tr("settings.project.renderer.restart", "Restart Now to Apply"), theme))
				host.ApplyProjectRendererChange(m_Startup.Renderer);
			y += 26.0f;
		}

		// 生效态说明:哪些立即生效、哪些要重启 —— 用户不该靠猜。
		{
			char buffer[256] = {};
			std::snprintf(buffer, sizeof(buffer),
				"生效中:剔除=%s 阴影=%s vsync=%s 合批=%s 贴图=%u 光=%u+%u",
				RenderSettings::CullingEnabled() ? "on" : "off",
				RenderSettings::ShadowsEnabled() ? "on" : "off",
				Renderer::IsVsyncEnabled() ? "on" : "off",
				RenderSettings::Get().Instancing ? "on" : "off",
				Renderer3D::GetShadowMapSize(),
				Renderer3D::GetMaxDirectionalLights(), Renderer3D::GetMaxPointLights());
			Wui::Label(ctx, { rect.X + pad, y }, buffer, theme.TextMuted, 12.0f);
			y += 18.0f;
			char physicsText[160] = {};
			std::snprintf(physicsText, sizeof(physicsText), "生效中物理:步长=%u Hz 重力=%.2f 各向异性=%u",
				PhysicsSettings::Get().FixedStepHz, PhysicsSettings::Get().Gravity,
				RenderSettings::Get().Anisotropy);
			Wui::Label(ctx, { rect.X + pad, y }, physicsText, theme.TextMuted, 12.0f);
			y += 18.0f;
		}

		Wui::WuiAccessNode statusNode;
		statusNode.Id = Wui::HashId("settings.status");
		statusNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		statusNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		statusNode.Kind = "status";
		statusNode.Label = "project settings status";
		statusNode.Value = m_Status;
		statusNode.Rect = { rect.X + pad, y, rect.W - pad * 2.0f, 16.0f };
		statusNode.Interactive = false;
		Wui::WuiAccessibility::Get().Register(statusNode);
		Wui::Label(ctx, { rect.X + pad, y }, m_Status,
			m_StatusIsError ? Wui::WuiColor { 0.97f, 0.31f, 0.29f, 1.0f } : theme.TextMuted, 12.0f);
	}
}
