#include "wldpch.h"
#include "World/Level/Authoring/LevelPackage.h"
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace World::Level
{
	struct LevelPackage::Impl
	{
		std::string Name;
		uint32_t GridCellSize{ 64 };
		std::vector<DataLayerDefinition> DataLayers;
		std::vector<EntityDescriptor> Entities;
	};

	LevelPackage::LevelPackage()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	LevelPackage::~LevelPackage() = default;
LevelPackage::LevelPackage(LevelPackage&& other) noexcept = default;
LevelPackage& LevelPackage::operator=(LevelPackage&& other) noexcept = default;

	const std::string& LevelPackage::GetName() const { return m_Impl->Name; }
	void LevelPackage::SetName(const std::string& name) { m_Impl->Name = name; }

	uint32_t LevelPackage::GetGridCellSize() const { return m_Impl->GridCellSize; }
	void LevelPackage::SetGridCellSize(uint32_t size) { m_Impl->GridCellSize = size; }

	const std::vector<DataLayerDefinition>& LevelPackage::GetDataLayers() const { return m_Impl->DataLayers; }
	std::vector<DataLayerDefinition>& LevelPackage::GetDataLayers() { return m_Impl->DataLayers; }

	size_t LevelPackage::GetEntityCount() const { return m_Impl->Entities.size(); }
	const EntityDescriptor& LevelPackage::GetEntity(size_t index) const { return m_Impl->Entities[index]; }
	EntityDescriptor& LevelPackage::GetMutableEntity(size_t index) { return m_Impl->Entities[index]; }

	void LevelPackage::AddEntity(const EntityDescriptor& entity)
	{
		for (auto& existing : m_Impl->Entities)
		{
			if (existing.EntityUUID == entity.EntityUUID)
			{
				existing = entity;
				return;
			}
		}
		m_Impl->Entities.push_back(entity);
	}

	const EntityDescriptor* LevelPackage::FindEntity(uint64_t entityUuid) const
	{
		for (const auto& entity : m_Impl->Entities)
		{
			if (entity.EntityUUID == entityUuid)
				return &entity;
		}
		return nullptr;
	}

	bool LevelPackage::Save(const std::filesystem::path& packageDir, std::string* outError) const
	{
		try
		{
			std::filesystem::create_directories(packageDir);

			YAML::Emitter manifest;
			manifest << YAML::BeginMap;
			manifest << YAML::Key << "Name" << YAML::Value << m_Impl->Name;
			manifest << YAML::Key << "GridCellSize" << YAML::Value << m_Impl->GridCellSize;

			if (!m_Impl->DataLayers.empty())
			{
				manifest << YAML::Key << "DataLayers" << YAML::Value << YAML::BeginSeq;
				for (const auto& layer : m_Impl->DataLayers)
				{
					manifest << YAML::BeginMap;
					manifest << YAML::Key << "Mask" << YAML::Value << layer.LayerMask;
					manifest << YAML::Key << "Name" << YAML::Value << layer.Name;
					manifest << YAML::Key << "DefaultActive" << YAML::Value << layer.DefaultActive;
					manifest << YAML::EndMap;
				}
				manifest << YAML::EndSeq;
			}
			manifest << YAML::EndMap;

			const std::filesystem::path manifestPath = packageDir / "manifest.yaml";
			std::ofstream fout(manifestPath);
			if (!fout.is_open())
			{
				if (outError) *outError = "Failed to open manifest for write: " + manifestPath.string();
				return false;
			}
			fout << manifest.c_str();
			fout.close();

			const std::filesystem::path entitiesDir = packageDir / "entities";
			std::filesystem::create_directories(entitiesDir);

			for (const auto& entity : m_Impl->Entities)
			{
				const std::string filename = std::to_string(entity.EntityUUID) + ".entity";
				const std::filesystem::path entityPath = entitiesDir / filename;
				if (!entity.Save(entityPath, outError))
					return false;
			}

			return true;
		}
		catch (const std::exception& e)
		{
			if (outError) *outError = e.what();
			return false;
		}
	}

	bool LevelPackage::Load(const std::filesystem::path& packageDir, std::string* outError)
	{
		if (!std::filesystem::exists(packageDir))
		{
			if (outError) *outError = "Package directory not found: " + packageDir.string();
			return false;
		}

		try
		{
			const std::filesystem::path manifestPath = packageDir / "manifest.yaml";
			if (!std::filesystem::exists(manifestPath))
			{
				if (outError) *outError = "manifest.yaml missing in package: " + packageDir.string();
				return false;
			}

			YAML::Node root = YAML::LoadFile(manifestPath.string());
			if (root["Name"]) m_Impl->Name = root["Name"].as<std::string>();
			if (root["GridCellSize"]) m_Impl->GridCellSize = root["GridCellSize"].as<uint32_t>();

			m_Impl->DataLayers.clear();
			if (YAML::Node layers = root["DataLayers"])
			{
				for (const auto& layerNode : layers)
				{
					DataLayerDefinition def;
					if (layerNode["Mask"]) def.LayerMask = layerNode["Mask"].as<uint32_t>();
					if (layerNode["Name"]) def.Name = layerNode["Name"].as<std::string>();
					if (layerNode["DefaultActive"]) def.DefaultActive = layerNode["DefaultActive"].as<bool>();
					m_Impl->DataLayers.push_back(def);
				}
			}

			m_Impl->Entities.clear();
			const std::filesystem::path entitiesDir = packageDir / "entities";
			if (std::filesystem::exists(entitiesDir))
			{
				for (const auto& entry : std::filesystem::directory_iterator(entitiesDir))
				{
					if (entry.is_regular_file() && entry.path().extension() == ".entity")
					{
						EntityDescriptor entity;
						if (!entity.Load(entry.path(), outError))
							return false;
						m_Impl->Entities.push_back(entity);
					}
				}
			}

			return true;
		}
		catch (const std::exception& e)
		{
			if (outError) *outError = e.what();
			return false;
		}
	}
}
