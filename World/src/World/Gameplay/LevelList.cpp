#include "wldpch.h"
#include "World/Gameplay/LevelList.h"

#include <yaml-cpp/yaml.h>

namespace World::Gameplay
{
	bool LevelList::Load(const std::filesystem::path& path, LevelList* out, std::string* error)
	{
		if (!out)
			return false;
		if (!std::filesystem::exists(path))
		{
			if (error) *error = "level list not found: " + path.string();
			return false;
		}

		try
		{
			const YAML::Node root = YAML::LoadFile(path.string());
			LevelList parsed;
			if (const YAML::Node levels = root["levels"])
			{
				for (const YAML::Node& item : levels)
				{
					LevelEntry entry;
					if (item["id"]) entry.Id = item["id"].as<std::string>();
					if (item["name"]) entry.DisplayName = item["name"].as<std::string>();
					if (item["scene"]) entry.ScenePath = item["scene"].as<std::string>();
					if (const YAML::Node packages = item["packages"])
						for (const YAML::Node& package : packages)
							entry.Packages.push_back(package.as<std::string>());

					if (entry.Id.empty() || entry.ScenePath.empty())
					{
						if (error) *error = "level entry requires both id and scene: " + path.string();
						return false;
					}
					if (entry.DisplayName.empty())
						entry.DisplayName = entry.Id;
					if (parsed.Find(entry.Id))
					{
						if (error) *error = "duplicate level id '" + entry.Id + "' in " + path.string();
						return false;
					}
					parsed.m_Entries.push_back(std::move(entry));
				}
			}

			*out = std::move(parsed);
			if (error) error->clear();
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error) *error = std::string("level list parse failed: ") + exception.what();
			return false;
		}
	}

	bool LevelList::Save(const std::filesystem::path& path, const LevelList& list, std::string* error)
	{
		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "levels" << YAML::Value << YAML::BeginSeq;
			for (const LevelEntry& entry : list.m_Entries)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "id" << YAML::Value << entry.Id;
				out << YAML::Key << "name" << YAML::Value << entry.DisplayName;
				out << YAML::Key << "scene" << YAML::Value << entry.ScenePath;
				out << YAML::Key << "packages" << YAML::Value << YAML::BeginSeq;
				for (const std::string& package : entry.Packages)
					out << package;
				out << YAML::EndSeq;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;

			std::filesystem::create_directories(path.parent_path());
			std::ofstream file(path);
			if (!file)
			{
				if (error) *error = "cannot write level list: " + path.string();
				return false;
			}
			file << out.c_str();
			if (error) error->clear();
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error) *error = std::string("level list write failed: ") + exception.what();
			return false;
		}
	}

	const LevelEntry* LevelList::Find(const std::string& id) const
	{
		for (const LevelEntry& entry : m_Entries)
			if (entry.Id == id)
				return &entry;
		return nullptr;
	}
}
