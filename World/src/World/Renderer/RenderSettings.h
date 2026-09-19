#pragma once

#include "World/Core/Asset/ProjectManifest.h"

namespace World
{
	// P1b D8a2:运行时生效的渲染设置。
	//
	// 单一事实源是项目清单(`project.we.yaml` 的 `rendering:` 区块,见
	// `Asset::RenderingSettings`);启动时由宿主(Editor/Runtime)调用 `Apply` 装载。
	// 渲染器(SceneRenderer/Renderer3D)只读 `Get()`,不直接碰文件。
	//
	// 优先级:**环境变量 > 清单 > 内置默认**。环境变量保留给自动化/诊断
	// (`WLD_NO_CULL=1` 关剔除、`WLD_NO_SHADOWS=1` 关阴影),它们的语义是
	// "本次运行强制",不写回清单。
	class WLD_API RenderSettings
	{
	public:
		// 清单 → 全局生效值(叠加环境变量覆盖)。
		static void Apply(const Asset::ProjectManifest& manifest);
		// 从工作目录定位并加载项目清单(project.we.yaml / Game/project.we.yaml)→ Apply;
		// 找不到/读不动时退回内置默认。渲染器初始化(Init)时自动调一次,保证
		// "清单改了就生效"对 Editor/Runtime/测试三条路径一致。
		static bool LoadFromProject(const std::filesystem::path& workingDirectory);
		// 直接设置(编辑器面板做即时预览;写盘由编辑器负责)。
		static void Set(const Asset::RenderingSettings& settings);
		// 当前生效值。
		static const Asset::RenderingSettings& Get();
		// 回到内置默认(测试/宿主未加载清单时)。
		static void ResetToDefaults();

		// 便捷读法(带环境变量覆盖的结果,等价于 Get().Culling)。
		static bool CullingEnabled();
		static bool ShadowsEnabled();
		// P4-3:场景渲染目标倍率(只缩放 SceneRenderer 的离屏目标;窗口/UI 不变)。
		// 清单加载期已校验范围,便捷读法返回原值,夹取由消费方(SceneRenderer)负责。
		static float RenderScale();
	};
}
