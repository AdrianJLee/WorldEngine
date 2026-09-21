#pragma once

#include "World/Core/Asset/ModelImportSettings.h"
#include "World/Core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World::Asset
{
	// 项目级渲染设置(project.we.yaml 的 `rendering:` 区块)。
	//
	// 为什么放在清单里:这些开关/上限过去只藏在代码常量或环境变量里,引擎用户
	// (做游戏的人)无法在不改引擎、不设环境变量的前提下调整。清单是项目的单一
	// 事实源,编辑器里也有面板可改(见 Editor 的 settings 面板)。
	//
	// 约束(ValidateManifest 会拒绝越界值):
	//  - ShadowMapSize:2 的幂,256..4096;
	//  - MaxDirectionalLights:1..2;MaxPointLights:0..7;两者之和 ≤ 8(UBO 容量)。
	//  - RenderScale(P4-3):0.25..2.0。
	struct RenderingSettings
	{
		bool Culling = true;              // 视锥剔除(主通道按相机、阴影通道按光源)
		bool Shadows = true;              // 方向光阴影通道
		uint32_t ShadowMapSize = 2048;    // 阴影贴图边长(启动时生效)
		uint32_t MaxDirectionalLights = 1;
		uint32_t MaxPointLights = 7;
		bool GpuTiming = false;           // D8b:GPU 时间戳(Stats 面板/AI 显示 gpuMs;有读回开销)
		// P4-perf:垂直同步(交换链呈现模式)。true = FIFO(vsync,防撕裂,默认);
		// false = Immediate(不等 vblank,帧率最高、可能撕裂)。
		// 运行期可切:编辑器 Project Settings 改动会重建交换链,不需要重启。
		// 开发覆盖:`WLD_VK_PRESENT_MODE=fifo|mailbox|immediate`(只影响本次运行)。
		bool Vsync = true;
		bool Instancing = true;           // D8b:同网格+材质的实例合批
		// P4-1:纹理各向异性上限(1..16;设备不支持时引擎退化为 1 并 warn)。
		uint32_t Anisotropy = 1;
		// P4-4b:MSAA 采样数(1/2/4/8;其它值在加载期拒绝)。1 = 关闭,默认值与旧行为
		// 逐字节一致(不建多采样附件、无 resolve)。作用范围是**场景渲染通道**的三类附件
		// (颜色 / 实体 id / 深度)与使用它的管线(Renderer2D/3D 及两个预览面板)。
		// 启动期参数:渲染器 Init 读一次(改值重启生效,与 shadow_map_size 同款口径);
		// 设备上限更低时引擎按颜色/整数颜色两类上限取交集、向下取 2 的幂并 warn 一次。
		uint32_t Msaa = 1;
		// P4-4b:msaa 的合法取值(清单校验、编辑器面板控件与渲染器共用同一口径)。
		static constexpr bool IsValidMsaa(uint32_t samples)
		{
			return samples == 1 || samples == 2 || samples == 4 || samples == 8;
		}
		// P4-3:渲染分辨率倍率。只缩放**场景渲染目标**(SceneRenderer 的离屏颜色/
		// 实体 id/深度附件):窗口尺寸、WUI/UI 与编辑器视口在屏幕上的占位都不变,
		// 视口把缩放后的纹理拉伸显示(与显示任意尺寸目标同一条路径)。默认 1.0 =
		// 与旧行为逐字节一致(同一尺寸、同一条路径)。
		float RenderScale = 1.0f;
		// P4-3:render_scale 的合法范围。清单校验、编辑器面板控件与渲染器共用同一口径,
		// 避免三处各写一份数字后漂移。
		static constexpr float MinRenderScale = 0.25f;
		static constexpr float MaxRenderScale = 2.0f;
	};

	// P4-1:物理设置(project.we.yaml 的 `physics:` 区块)。
	struct PhysicsSettingsData
	{
		uint32_t FixedStepHz = 60;   // 固定步长频率(1..240)
		float Gravity = -9.81f;      // 重力加速度(Y 轴,有限值)
	};

	// 项目清单(project.we.yaml):资产内容根、启动场景与发行包列表的单一事实源。
	// ContentRoot 相对 manifest 文件所在目录;Packages 为发行布局下的相对路径。
	class WLD_API ProjectManifest
	{
	public:
		std::string Id;
		std::string Version = "1.0.0";
		std::filesystem::path ContentRoot = "assets";
		std::string StartScene;
		std::string Renderer = "opengl";   // opengl | vulkan
		std::vector<std::string> Packages;
		// D8a2:渲染设置(缺省 = 引擎默认;`rendering:` 区块缺失时保持默认)。
		RenderingSettings Rendering;
		// P4-1:物理设置(缺省 = 引擎默认;`physics:` 区块缺失时保持默认)。
		PhysicsSettingsData Physics;
		// P4-U4(2026-09-21):资产导入默认值(`imports:` 区块)。作为"源还没有产物时"的
		// 模型导入默认 —— 已有资产 meta 里的逐源设置优先(ModelImportSettings::ResolveForImport)。
		ModelImportSettings ImportDefaults;

		// 加载并校验;error 为空表示成功。
		static bool Load(const std::filesystem::path& path, ProjectManifest* out, std::string* error);
		// 序列化(ContentRoot 始终写相对路径,与清单文件目录无关)。
		static bool Save(const std::filesystem::path& path, const ProjectManifest& manifest, std::string* error);

		// 相对 manifest 文件目录解析内容根绝对路径。
		std::filesystem::path ResolveContentRoot(const std::filesystem::path& manifestPath) const;
		// 按工作目录依次尝试 project.we.yaml、Game/project.we.yaml;命中则 out 填实际路径。
		static bool Locate(const std::filesystem::path& workingDirectory, std::filesystem::path* manifestPath);
	};
}
