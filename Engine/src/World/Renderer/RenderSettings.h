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
		// P4-perf:垂直同步(清单 `rendering.vsync`)。消费方是 Renderer 的交换链创建
		// (Fifo/Immediate)与 GL 侧 swap interval;运行期切换走 `Renderer::SetVsync`。
		static bool VsyncEnabled();
		// P4-3:场景渲染目标倍率(只缩放 SceneRenderer 的离屏目标;窗口/UI 不变)。
		// 清单加载期已校验范围,便捷读法返回原值,夹取由消费方(SceneRenderer)负责。
		static float RenderScale();
		// P4-4b:MSAA **生效采样数**(1/2/4/8)。
		//
		// 口径:
		//  - 请求值来自清单 `rendering.msaa`(加载期已校验 ∈ {1,2,4,8});
		//  - 生效值 = min(请求值, 设备 MaxSampleCount, 设备 MaxIntegerSampleCount)
		//    再向下取到 2 的幂(整数颜色上限来自实体 id 附件:R32_SINT);
		//  - 请求值 1(默认)直接返回 1:不查询设备、不告警、不创建任何资源,保证
		//    msaa==1 的运行路径与旧行为逐字节一致;
		//  - 设备上限把值压低时 `WLD_CORE_WARN` 一次;
		//  - **有设备时按设备冻结**:渲染器只在 Init 里建一次渲染通道/管线,而预览面板
		//    可能到运行中才第一次创建自己的通道;两者必须拿到同一个采样数,否则 Vulkan
		//    判定通道不兼容。因此首个生效值在设备生命周期内固定,清单改值要重启生效
		//    (与 shadow_map_size 同款"启动期参数"口径)。设备重建会重算。
		//  - 设备未就绪(纯逻辑测试)时退化为"请求值 → 设备无关夹取"的纯函数。
		static uint32_t Msaa();
	};
}
