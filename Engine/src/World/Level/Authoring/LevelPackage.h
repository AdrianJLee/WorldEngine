#pragma once

#include "World/Level/Authoring/EntityDescriptor.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace World::Level
{
	struct DataLayerDefinition
	{
		uint32_t LayerMask{ 1 };
		std::string Name;
		bool DefaultActive{ true };
	};

	class LevelPackage
	{
	public:
		LevelPackage();
		~LevelPackage();

		const std::string& GetName() const;
		void SetName(const std::string& name);

		uint32_t GetGridCellSize() const;
		void SetGridCellSize(uint32_t size);

		const std::vector<DataLayerDefinition>& GetDataLayers() const;
		std::vector<DataLayerDefinition>& GetDataLayers();

		size_t GetEntityCount() const;
		const EntityDescriptor& GetEntity(size_t index) const;
		void AddEntity(const EntityDescriptor& entity);
		const EntityDescriptor* FindEntity(uint64_t entityUuid) const;

		bool Save(const std::filesystem::path& packageDir, std::string* outError = nullptr) const;
		bool Load(const std::filesystem::path& packageDir, std::string* outError = nullptr);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
