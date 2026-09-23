#include "wldpch.h"
#include "World/Core/PhysicsSettings.h"

#include <filesystem>

namespace World
{
	namespace
	{
		Asset::PhysicsSettingsData& Store()
		{
			static Asset::PhysicsSettingsData settings;
			return settings;
		}
	}

	void PhysicsSettings::Apply(const Asset::ProjectManifest& manifest)
	{
		Store() = manifest.Physics;
	}

	void PhysicsSettings::Set(const Asset::PhysicsSettingsData& settings)
	{
		Store() = settings;
	}

	const Asset::PhysicsSettingsData& PhysicsSettings::Get()
	{
		return Store();
	}

	void PhysicsSettings::ResetToDefaults()
	{
		Store() = Asset::PhysicsSettingsData {};
	}

	bool PhysicsSettings::LoadFromProject(const std::filesystem::path& workingDirectory)
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
			WLD_CORE_WARN("物理设置:项目清单加载失败,使用默认值 ({0}): {1}", manifestPath.string(), error);
			ResetToDefaults();
			return false;
		}
		Apply(manifest);
		return true;
	}
}
