#pragma once

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace World::Level
{
	struct EntityProperty
	{
		std::string Key;
		std::string Value;
	};

	struct EntityDescriptor
	{
		uint64_t EntityUUID{ 0 };
		std::string Name;
		uint64_t ChunkID{ 0 };
		uint32_t DataLayerMask{ 1 };

		glm::vec3 Position{ 0.0f };
		glm::vec3 RotationEuler{ 0.0f };
		glm::vec3 Scale{ 1.0f };

		std::vector<EntityProperty> Properties;

		bool Save(const std::filesystem::path& filepath, std::string* outError = nullptr) const;
		bool Load(const std::filesystem::path& filepath, std::string* outError = nullptr);

		void SetProperty(const std::string& key, const std::string& val);
		std::string GetProperty(const std::string& key, const std::string& fallback = "") const;
		bool HasProperty(const std::string& key) const;
	};
}
