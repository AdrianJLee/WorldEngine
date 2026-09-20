#include "wldpch.h"
#include "SettingsPanel.h"

#include "../SettingsUi.h"
#include "World/Core/PhysicsSettings.h"
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
	// 兼容性:迁移过来的行带 `AccessId`(= 迁移前的老节点 id,如 `settings3d.msaa`),
	// 既有 AI 脚本/自动化按老 id 读写仍然有效;本地化键沿用既有 `settings.<id>`。
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
			const char* label, const char* tooltip, const char* accessId)
		{
			SettingDescriptor descriptor;
			descriptor.Id = id;
			descriptor.AccessId = accessId ? accessId : "";
			descriptor.Group = group;
			descriptor.Type = type;
			descriptor.Scope = SettingScope::Project;
			descriptor.Apply = apply;
			descriptor.Label = label;
			descriptor.Tooltip = tooltip;
			return descriptor;
		};

		auto addBool = [&registry, &describe](const char* id, const char* group, const char* label,
			const char* tooltip, const char* accessId, ReadFn read, WriteFn write, FlagFn isDefault,
			FlagFn reset = FlagFn {})
		{
			SettingDescriptor descriptor = describe(id, group, SettingType::Bool, SettingApply::Immediate,
				label, tooltip, accessId);
			descriptor.Read = std::move(read);
			descriptor.Write = std::move(write);
			descriptor.IsDefault = std::move(isDefault);
			descriptor.Reset = std::move(reset);
			registry.Register(std::move(descriptor));
		};

		auto addNumber = [&registry, &describe](const char* id, const char* group, SettingType type,
			SettingApply apply, const char* label, const char* tooltip, const char* accessId, const char* unit,
			double min, double max, double step, ReadFn read, WriteFn write, FlagFn isDefault, FlagFn reset)
		{
			SettingDescriptor descriptor = describe(id, group, type, apply, label, tooltip, accessId);
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
			const char* label, const char* tooltip, const char* accessId, std::vector<SettingOption> options,
			ReadFn read, WriteFn write, FlagFn isDefault, FlagFn reset)
		{
			SettingDescriptor descriptor = describe(id, group, SettingType::Enum, apply, label, tooltip, accessId);
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
			"视锥剔除\n不在相机视野内的物体不提交绘制。\n默认:开。立即生效。", "settings3d.culling",
			[this] { return BoolText(m_Edit.Culling); },
			[this](const std::string& value, std::string*) { m_Edit.Culling = value == "true"; return true; },
			[this] { return m_Edit.Culling == renderDefaults.Culling; },
			[this] { m_Edit.Culling = renderDefaults.Culling; return true; });

		addBool("shadows", "Rendering", "Directional Shadows",
			"平行光阴影\n主方向光渲染一张阴影贴图。\n默认:开。立即生效。", "settings3d.shadows",
			[this] { return BoolText(m_Edit.Shadows); },
			[this](const std::string& value, std::string*) { m_Edit.Shadows = value == "true"; return true; },
			[this] { return m_Edit.Shadows == renderDefaults.Shadows; },
			[this] { m_Edit.Shadows = renderDefaults.Shadows; return true; });

		addBool("vsync", "Rendering", "Vertical Sync",
			"垂直同步\n等待显示器刷新再呈现(防撕裂;会把帧率压到刷新率)。\n默认:开。立即生效(会重建交换链)。",
			"settings3d.vsync",
			[this] { return BoolText(m_Edit.Vsync); },
			[this](const std::string& value, std::string*) { m_Edit.Vsync = value == "true"; return true; },
			[this] { return m_Edit.Vsync == renderDefaults.Vsync; },
			[this] { m_Edit.Vsync = renderDefaults.Vsync; return true; });

		addEnum("shadow_map", "Rendering", SettingApply::Restart, "Shadow Map Size",
			"阴影贴图尺寸\n平行光阴影贴图的分辨率(512-4096,2 的幂)。\n下次启动生效。", "settings3d.shadow_map",
			{ SettingOption { "512", "512" }, SettingOption { "1024", "1024" },
				SettingOption { "2048", "2048" }, SettingOption { "4096", "4096" } },
			[this] { return std::to_string(m_Edit.ShadowMapSize); },
			[this](const std::string& value, std::string*) { m_Edit.ShadowMapSize = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.ShadowMapSize == renderDefaults.ShadowMapSize; },
			[this] { m_Edit.ShadowMapSize = renderDefaults.ShadowMapSize; return true; });

		addNumber("max_directional", "Rendering", SettingType::Int, SettingApply::Immediate, "Max Directional Lights",
			"平行光上限\n一帧打包多少盏平行光(1-2,与点光共享 8 盏的预算)。\n立即生效。", "settings3d.max_directional",
			"", 1.0, static_cast<double>(Renderer3D::MaxDirectionalLightCapacity), 1.0,
			[this] { return std::to_string(m_Edit.MaxDirectionalLights); },
			[this](const std::string& value, std::string*) { m_Edit.MaxDirectionalLights = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.MaxDirectionalLights == renderDefaults.MaxDirectionalLights; },
			[this] { m_Edit.MaxDirectionalLights = renderDefaults.MaxDirectionalLights; return true; });

		addNumber("max_point", "Rendering", SettingType::Int, SettingApply::Immediate, "Max Point Lights",
			"点光上限\n一帧打包多少盏点光(0-7,与平行光共享 8 盏的预算)。\n立即生效。", "settings3d.max_point",
			"", 0.0, static_cast<double>(Renderer3D::MaxPointLightCapacity), 1.0,
			[this] { return std::to_string(m_Edit.MaxPointLights); },
			[this](const std::string& value, std::string*) { m_Edit.MaxPointLights = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.MaxPointLights == renderDefaults.MaxPointLights; },
			[this] { m_Edit.MaxPointLights = renderDefaults.MaxPointLights; return true; });

		addNumber("anisotropy", "Rendering", SettingType::Int, SettingApply::Immediate, "Texture Anisotropy",
			"纹理各向异性\n场景贴图的最大各向异性(1-16,夹到设备上限,1 = 关闭)。\n立即生效。", "settings3d.anisotropy",
			"", 1.0, 16.0, 1.0,
			[this] { return std::to_string(m_Edit.Anisotropy); },
			[this](const std::string& value, std::string*) { m_Edit.Anisotropy = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.Anisotropy == renderDefaults.Anisotropy; },
			[this] { m_Edit.Anisotropy = renderDefaults.Anisotropy; return true; });

		addNumber("render_scale", "Rendering", SettingType::Float, SettingApply::Immediate, "Render Scale",
			"渲染分辨率倍率\n只缩放场景渲染目标(0.25-2.0),窗口与 UI 不变。\n立即生效。", "settings3d.render_scale",
			"x", 0.25, 2.0, 0.05,
			[this] { return FormatFloat(m_Edit.RenderScale); },
			[this](const std::string& value, std::string*) { m_Edit.RenderScale = static_cast<float>(std::atof(value.c_str())); return true; },
			[this] { return m_Edit.RenderScale == renderDefaults.RenderScale; },
			[this] { m_Edit.RenderScale = renderDefaults.RenderScale; return true; });

		addEnum("msaa", "Rendering", SettingApply::Restart, "MSAA (restart)",
			"多重采样\n场景通道的采样数(1/2/4/8)。管线与预览通道在启动时创建,需要重启。",
			"settings3d.msaa",
			{ SettingOption { "1", "1" }, SettingOption { "2", "2" },
				SettingOption { "4", "4" }, SettingOption { "8", "8" } },
			[this] { return std::to_string(m_Edit.Msaa); },
			[this](const std::string& value, std::string*) { m_Edit.Msaa = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Edit.Msaa == renderDefaults.Msaa; },
			[this] { m_Edit.Msaa = renderDefaults.Msaa; return true; });

		addBool("instancing", "Rendering", "GPU Instancing",
			"GPU 实例合批\n同网格 + 同材质的物体合批绘制(重复道具收益最大)。\n默认:开。立即生效;关掉可做 A/B 对照。",
			"settings3d.instancing",
			[this] { return BoolText(m_Edit.Instancing); },
			[this](const std::string& value, std::string*) { m_Edit.Instancing = value == "true"; return true; },
			[this] { return m_Edit.Instancing == renderDefaults.Instancing; },
			[this] { m_Edit.Instancing = renderDefaults.Instancing; return true; });

		addBool("gpu_timing", "Rendering", "GPU Timing",
			"GPU 计时\n用时间戳查询统计场景通道耗时,Stats 面板与 AI 通道可读 gpuMs。\n默认:关(读回有一点代价)。立即生效。",
			"settings3d.gpu_timing",
			[this] { return BoolText(m_Edit.GpuTiming); },
			[this](const std::string& value, std::string*) { m_Edit.GpuTiming = value == "true"; return true; },
			[this] { return m_Edit.GpuTiming == renderDefaults.GpuTiming; },
			[this] { m_Edit.GpuTiming = renderDefaults.GpuTiming; return true; });

		// ---- 物理 Physics ----
		addNumber("physics.fixed_step", "Physics", SettingType::Int, SettingApply::NextPlay, "Fixed Timestep",
			"固定步长\n物理更新频率 Hz(1-240)。下次 Play / Runtime 启动生效。\n默认:60。",
			"settings.physics.fixed_step", "Hz", 1.0, 240.0, 1.0,
			[this] { return std::to_string(m_Physics.FixedStepHz); },
			[this](const std::string& value, std::string*) { m_Physics.FixedStepHz = static_cast<uint32_t>(std::atoi(value.c_str())); return true; },
			[this] { return m_Physics.FixedStepHz == physicsDefaults.FixedStepHz; },
			[this] { m_Physics.FixedStepHz = physicsDefaults.FixedStepHz; return true; });

		addNumber("physics.gravity", "Physics", SettingType::Float, SettingApply::NextPlay, "Gravity",
			"重力\nY 轴加速度(m/s²)。下次 Play / Runtime 启动生效。\n默认:-9.81。",
			"settings.physics.gravity", "", -50.0, 50.0, 0.1,
			[this] { return FormatFloat(m_Physics.Gravity); },
			[this](const std::string& value, std::string*) { m_Physics.Gravity = static_cast<float>(std::atof(value.c_str())); return true; },
			[this] { return m_Physics.Gravity == physicsDefaults.Gravity; },
			[this] { m_Physics.Gravity = physicsDefaults.Gravity; return true; });

		// ---- 启动与内容 Startup(写清单是即时的,所以不给"恢复默认":它们不是"默认值"问题) ----
		addEnum("project.renderer", "Startup", SettingApply::Restart, "Renderer",
			"渲染后端\n编辑器与 Runtime 启动时使用的 RHI 后端(OpenGL / Vulkan)。\n"
			"运行中热切换会串资源,因此需要重启编辑器;改完面板会给「立即重启」入口。",
			"settings.project.renderer",
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
				"启动场景\n打包后的 Runtime 启动时加载的场景(路径相对内容根)。\n下次启动 Runtime / 打包时生效。",
				"settings.project.start_scene");
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
				"内容根\n存放项目资产的目录(相对 project.we.yaml)。\n挂载点在启动时建立,改完需要重启编辑器。",
				"settings.project.content_root");
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

	void SettingsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		m_Host = &host;
		if (!m_Initialized)
		{
			m_Edit = RenderSettings::Get();
			m_Physics = PhysicsSettings::Get();
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
		const Wui::WuiRect page { rect.X + pad, rect.Y + pad, rect.W - pad * 2.0f,
			std::max(120.0f, rect.H - pad * 2.0f - 116.0f) };
		const std::vector<std::string> groups { "Rendering", "Physics", "Startup" };
		bool changed = Editor::DrawSettingsPage(ctx, page, theme, Settings::SettingScope::Project, groups, m_Page);

		// 手写部分:发行包列表(project.we.yaml 的 `packages:`)。需要真正的"列表编辑"而不是一行文本,
		// 所以不进注册表(注册表是"一行一个值"的模型)。
		float y = page.Y + page.H + 6.0f;
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
				host.SaveProjectPhysicsSettings(m_Physics, &message))
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
