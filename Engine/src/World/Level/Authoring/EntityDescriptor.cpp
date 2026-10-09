#include "wldpch.h"
#include "World/Level/Authoring/EntityDescriptor.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

namespace World::Level
{
	namespace
	{
		constexpr const char* kEntityArrayKey = "Entities";
		constexpr const char* kUuidComponentKey = "World::UUIDComponent";
		constexpr const char* kTagComponentKey = "World::TagComponent";
		constexpr const char* kTransformComponentKey = "World::TransformComponent";

		void ReadVec3(const YAML::Node& node, glm::vec3* out)
		{
			if (!node || !node.IsSequence() || node.size() != 3)
				return;
			out->x = node[0].as<float>(out->x);
			out->y = node[1].as<float>(out->y);
			out->z = node[2].as<float>(out->z);
		}
	}

	void EntityDescriptor::RefreshDerivedFromDocument()
	{
		if (DocumentYaml.empty())
			return;

		YAML::Node document;
		try
		{
			document = YAML::Load(DocumentYaml);
		}
		catch (const std::exception&)
		{
			return;
		}

		const YAML::Node entities = document[kEntityArrayKey];
		if (!entities || !entities.IsSequence() || entities.size() == 0)
			return;
		const YAML::Node entity = entities[0];
		if (!entity || !entity.IsMap())
			return;

		if (const YAML::Node identity = entity[kUuidComponentKey])
			if (const YAML::Node id = identity["ID"])
				if (const YAML::Node raw = id["m_UUID"])
					EntityUUID = raw.as<uint64_t>(EntityUUID);

		if (const YAML::Node tag = entity[kTagComponentKey])
			if (const YAML::Node text = tag["Tag"])
				Name = text.as<std::string>(Name);

		if (const YAML::Node transform = entity[kTransformComponentKey])
		{
			ReadVec3(transform["Location"], &Position);
			ReadVec3(transform["Rotation"], &RotationEuler);
			Scale = glm::vec3(1.0f);
			ReadVec3(transform["Scale"], &Scale);
		}
	}

	bool EntityDescriptor::Save(const std::filesystem::path& filepath, std::string* outError) const
	{
		try
		{
			if (DocumentYaml.empty())
			{
				if (outError) *outError = "entity descriptor has no document payload";
				return false;
			}

			const YAML::Node document = YAML::Load(DocumentYaml);
			const YAML::Node entities = document[kEntityArrayKey];
			if (!entities || !entities.IsSequence() || entities.size() == 0)
			{
				if (outError) *outError = "entity document payload has no entity";
				return false;
			}

			// 落盘形状:关卡元数据(不在组件里)+ 与 .wd 同形的实体组件块。
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "ChunkID" << YAML::Value << ChunkID;
			out << YAML::Key << "DataLayerMask" << YAML::Value << DataLayerMask;
			out << YAML::Key << "Entity" << YAML::Value << entities[0];
			out << YAML::EndMap;

			if (filepath.has_parent_path())
				std::filesystem::create_directories(filepath.parent_path());

			std::ofstream fout(filepath);
			if (!fout.is_open())
			{
				if (outError) *outError = "cannot open for write: " + filepath.string();
				return false;
			}
			fout << out.c_str();
			return true;
		}
		catch (const std::exception& e)
		{
			if (outError) *outError = e.what();
			return false;
		}
	}

	bool EntityDescriptor::Load(const std::filesystem::path& filepath, std::string* outError)
	{
		if (!std::filesystem::is_regular_file(filepath))
		{
			if (outError) *outError = "entity file not found: " + filepath.string();
			return false;
		}

		try
		{
			const YAML::Node root = YAML::LoadFile(filepath.string());
			if (!root || !root.IsMap())
			{
				if (outError) *outError = "entity file is not a map: " + filepath.string();
				return false;
			}

			ChunkID = root["ChunkID"] ? root["ChunkID"].as<uint64_t>(0) : 0;
			DataLayerMask = root["DataLayerMask"] ? root["DataLayerMask"].as<uint32_t>(1) : 1u;

			const YAML::Node entity = root["Entity"];
			if (!entity || !entity.IsMap())
			{
				if (outError) *outError = "entity file has no 'Entity' block: " + filepath.string();
				return false;
			}

			// 还原成单实体场景文档,让权威载荷与 .wd 实体块保持同一形状。
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << kEntityArrayKey << YAML::Value << YAML::BeginSeq << entity << YAML::EndSeq;
			out << YAML::EndMap;
			DocumentYaml = out.c_str();

			RefreshDerivedFromDocument();
			return true;
		}
		catch (const std::exception& e)
		{
			if (outError) *outError = e.what();
			return false;
		}
	}
}
