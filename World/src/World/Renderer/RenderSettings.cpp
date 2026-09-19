#include "wldpch.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Core/PhysicsSettings.h"

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
}
