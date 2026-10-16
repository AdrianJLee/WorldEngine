#include "wldpch.h"
#include "World/Physics/PhysicsSettings.h"

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

	const Asset::CollisionChannelConfig& PhysicsSettings::GetChannels()
	{
		return Store().Channels;
	}

	Asset::CollisionChannelConfig& PhysicsSettings::MutableChannels()
	{
		return Store().Channels;
	}

	std::string_view PhysicsSettings::GetChannelName(uint8_t index)
	{
		return Store().Channels.GetChannelName(index);
	}

	bool PhysicsSettings::SetChannelName(uint8_t index, std::string_view name)
	{
		return Store().Channels.SetChannelName(index, name);
	}

	bool PhysicsSettings::GetChannelCollision(uint8_t channelA, uint8_t channelB)
	{
		return Store().Channels.GetCollision(channelA, channelB);
	}

	void PhysicsSettings::SetChannelCollision(uint8_t channelA, uint8_t channelB, bool collide)
	{
		Store().Channels.SetCollision(channelA, channelB, collide);
	}

	uint32_t PhysicsSettings::GetMaskForChannel(uint8_t channel)
	{
		return Store().Channels.GetMask(channel);
	}

	int PhysicsSettings::FindChannelIndex(std::string_view name)
	{
		return Store().Channels.FindChannelIndex(name);
	}
}
