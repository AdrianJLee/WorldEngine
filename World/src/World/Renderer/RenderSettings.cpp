#include "wldpch.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Core/PhysicsSettings.h"
#include "World/Renderer/Renderer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace World
{
	namespace
	{
		Asset::RenderingSettings& Store()
		{
			static Asset::RenderingSettings settings;
			return settings;
		}

		bool EnvironmentFlag(const char* name)
		{
			const char* value = std::getenv(name);
			return value != nullptr && value[0] != '\0' && value[0] != '0';
		}

		// 环境变量只在"关"的方向上覆盖(自动化/诊断用),不提供"强制打开"——
		// 否则测试脚本的 WLD_* 会把用户在清单里关掉的功能偷偷打开。
		Asset::RenderingSettings WithEnvironmentOverrides(Asset::RenderingSettings settings)
		{
			if (EnvironmentFlag("WLD_NO_CULL"))
				settings.Culling = false;
			if (EnvironmentFlag("WLD_NO_SHADOWS"))
				settings.Shadows = false;
			return settings;
		}
	}

	void RenderSettings::Apply(const Asset::ProjectManifest& manifest)
	{
		Store() = WithEnvironmentOverrides(manifest.Rendering);
		// P4-1:物理设置与渲染设置同源(都来自项目清单),绑定在同一个入口上,
		// 这样 Editor / Runtime / Renderer3D::Init 三条装载路径都不会漏掉物理。
		PhysicsSettings::Apply(manifest);
	}

	bool RenderSettings::LoadFromProject(const std::filesystem::path& workingDirectory)
	{
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(workingDirectory, &manifestPath))
		{
			ResetToDefaults();
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			WLD_CORE_WARN("渲染设置:项目清单加载失败,使用默认值 ({0}): {1}", manifestPath.string(), error);
			ResetToDefaults();
			return false;
		}
		Apply(manifest);
		return true;
	}

	void RenderSettings::Set(const Asset::RenderingSettings& settings)
	{
		Store() = WithEnvironmentOverrides(settings);
	}

	const Asset::RenderingSettings& RenderSettings::Get()
	{
		return Store();
	}

	void RenderSettings::ResetToDefaults()
	{
		Store() = WithEnvironmentOverrides(Asset::RenderingSettings {});
	}

	bool RenderSettings::CullingEnabled()
	{
		return Store().Culling;
	}

	bool RenderSettings::ShadowsEnabled()
	{
		return Store().Shadows;
	}

	float RenderSettings::RenderScale()
	{
		return Store().RenderScale;
	}

	uint32_t RenderSettings::Msaa()
	{
		const uint32_t requested = Store().Msaa;
		// 默认/关闭:不进设备查询、不告警、不冻结 —— msaa==1 与旧路径逐字节一致。
		if (requested <= 1)
			return 1;
		const Rhi::Handle<Rhi::Device> device = Renderer::GetDevice();
		// 设备未就绪(纯逻辑测试/资源未初始化):返回请求值本身(加载期已校验 1/2/4/8)。
		if (!device)
			return requested;

		// 有设备时**按设备冻结**首个生效值:渲染通道/管线只在 Init 建一次,而预览面板
		// 可能到运行中才第一次创建自己的通道 —— 两边必须用同一个采样数(Vulkan 的渲染
		// 通道兼容性逐项比较采样数与 resolve 引用)。清单改值重启生效(见头文件口径)。
		static const void* frozenDevice = nullptr;
		static uint32_t frozenValue = 1;
		if (frozenDevice == device.get())
			return frozenValue;

		const Rhi::Capabilities& capabilities = device->GetCapabilities();
		// 两类上限取交集:颜色附件(R8G8B8A8_UNORM)与整数颜色附件(R32_SINT 实体 id)。
		uint32_t limit = std::min(requested, std::max(1u, capabilities.MaxSampleCount));
		limit = std::min(limit, std::max(1u, capabilities.MaxIntegerSampleCount));
		// 向下取到 2 的幂(设备上限不保证是 2 的幂;请求值本身已是 1/2/4/8)。
		uint32_t effective = 1;
		while (effective * 2 <= limit)
			effective *= 2;
		if (effective != requested)
			WLD_CORE_WARN("rendering.msaa {0} 被设备上限压低为 {1}(颜色上限 {2} / 整数颜色上限 {3})",
				requested, effective, capabilities.MaxSampleCount, capabilities.MaxIntegerSampleCount);
		frozenDevice = device.get();
		frozenValue = effective;
		return effective;
	}
}
