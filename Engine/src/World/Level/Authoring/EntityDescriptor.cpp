#include "wldpch.h"
#include "World/Level/Authoring/EntityDescriptor.h"
#include <algorithm>
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace World::Level
{
	void EntityDescriptor::SetProperty(const std::string& key, const std::string& val)
	{
		for (auto& prop : Properties)
		{
			if (prop.Key == key)
			{
				prop.Value = val;
				return;
			}
		}
		Properties.push_back({ key, val });
	}

	std::string EntityDescriptor::GetProperty(const std::string& key, const std::string& fallback) const
	{
		for (const auto& prop : Properties)
		{
			if (prop.Key == key)
				return prop.Value;
		}
		return fallback;
	}

	bool EntityDescriptor::HasProperty(const std::string& key) const
	{
		for (const auto& prop : Properties)
		{
			if (prop.Key == key)
				return true;
		}
		return false;
	}

	bool EntityDescriptor::Save(const std::filesystem::path& filepath, std::string* outError) const
	{
		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "EntityUUID" << YAML::Value << EntityUUID;
			out << YAML::Key << "Name" << YAML::Value << Name;
			out << YAML::Key << "ChunkID" << YAML::Value << ChunkID;
			out << YAML::Key << "DataLayerMask" << YAML::Value << DataLayerMask;

			out << YAML::Key << "Transform" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "Position" << YAML::Value << YAML::Flow << YAML::BeginSeq << Position.x << Position.y << Position.z << YAML::EndSeq;
			out << YAML::Key << "Rotation" << YAML::Value << YAML::Flow << YAML::BeginSeq << RotationEuler.x << RotationEuler.y << RotationEuler.z << YAML::EndSeq;
			out << YAML::Key << "Scale" << YAML::Value << YAML::Flow << YAML::BeginSeq << Scale.x << Scale.y << Scale.z << YAML::EndSeq;
			out << YAML::EndMap;

			if (!Properties.empty())
			{
				out << YAML::Key << "Properties" << YAML::Value << YAML::BeginMap;
				for (const auto& prop : Properties)
				{
					out << YAML::Key << prop.Key << YAML::Value << prop.Value;
				}
				out << YAML::EndMap;
			}

			out << YAML::EndMap;

			if (filepath.has_parent_path())
			{
				std::filesystem::create_directories(filepath.parent_path());
			}

			std::ofstream fout(filepath);
			if (!fout.is_open())
			{
				if (outError) *outError = "Failed to open file for write: " + filepath.string();
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
		if (!std::filesystem::exists(filepath))
		{
			if (outError) *outError = "File not found: " + filepath.string();
			return false;
		}

		try
		{
			YAML::Node root = YAML::LoadFile(filepath.string());
			if (root["EntityUUID"]) EntityUUID = root["EntityUUID"].as<uint64_t>();
			if (root["Name"]) Name = root["Name"].as<std::string>();
			if (root["ChunkID"]) ChunkID = root["ChunkID"].as<uint64_t>();
			if (root["DataLayerMask"]) DataLayerMask = root["DataLayerMask"].as<uint32_t>();

			if (YAML::Node transform = root["Transform"])
			{
				if (transform["Position"] && transform["Position"].size() == 3)
				{
					Position.x = transform["Position"][0].as<float>();
					Position.y = transform["Position"][1].as<float>();
					Position.z = transform["Position"][2].as<float>();
				}
				if (transform["Rotation"] && transform["Rotation"].size() == 3)
				{
					RotationEuler.x = transform["Rotation"][0].as<float>();
					RotationEuler.y = transform["Rotation"][1].as<float>();
					RotationEuler.z = transform["Rotation"][2].as<float>();
				}
				if (transform["Scale"] && transform["Scale"].size() == 3)
				{
					Scale.x = transform["Scale"][0].as<float>();
					Scale.y = transform["Scale"][1].as<float>();
					Scale.z = transform["Scale"][2].as<float>();
				}
			}

			Properties.clear();
			if (YAML::Node props = root["Properties"])
			{
				for (YAML::const_iterator it = props.begin(); it != props.end(); ++it)
				{
					Properties.push_back({ it->first.as<std::string>(), it->second.as<std::string>() });
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
