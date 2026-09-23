#pragma once

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Export.h"

namespace World
{
	// P4-1:运行时生效的物理设置(与 RenderSettings 同款:清单 `physics:` 区块是事实源)。
	// 环境变量不参与(物理没有需要自动化强制的开关)。
	class WLD_API PhysicsSettings
	{
	public:
		static void Apply(const Asset::ProjectManifest& manifest);
		static void Set(const Asset::PhysicsSettingsData& settings);
		static const Asset::PhysicsSettingsData& Get();
		static void ResetToDefaults();
		// 与 RenderSettings::LoadFromProject 一起调用(渲染器初始化时装载一次)。
		static bool LoadFromProject(const std::filesystem::path& workingDirectory);
	};
}
