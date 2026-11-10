#include "wldpch.h"
#include "World/Level/Core/ChunkIndex.h"

#include "World/Core/Log.h"

#include <fstream>
#include <yaml-cpp/yaml.h>

namespace World::Level
{
	std::filesystem::path ChunkIndex::ResolveFile(const std::filesystem::path& indexDir,
		const ChunkIndexEntry& entry) const
	{
		return indexDir / std::filesystem::u8path(entry.File);
	}

	bool ChunkIndex::Parse(const std::string& yaml, ChunkIndex* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "no output index";
			return false;
		}

		YAML::Node root;
		try
		{
			root = YAML::Load(yaml);
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}

		YAML::Node chunks = root["Chunks"];
		if (!chunks || !chunks.IsSequence())
		{
			if (error) *error = "chunk index has no 'Chunks' sequence";
			return false;
		}

		ChunkIndex parsed;
		for (const YAML::Node& node : chunks)
		{
			if (!node.IsMap())
				continue;

			ChunkIndexEntry entry;
			entry.ChunkID = node["Id"] ? node["Id"].as<uint64_t>(0) : 0;
			entry.File = node["File"] ? node["File"].as<std::string>("") : std::string();
			entry.EntityCount = node["Entities"] ? node["Entities"].as<uint32_t>(0) : 0u;
			entry.DataLayerMask = node["DataLayerMask"] ? node["DataLayerMask"].as<uint32_t>(0) : 0u;

			if (const YAML::Node min = node["Min"])
				if (min.IsSequence() && min.size() == 3)
					entry.Bounds.Min = { min[0].as<float>(0.0f), min[1].as<float>(0.0f), min[2].as<float>(0.0f) };
			if (const YAML::Node max = node["Max"])
				if (max.IsSequence() && max.size() == 3)
					entry.Bounds.Max = { max[0].as<float>(0.0f), max[1].as<float>(0.0f), max[2].as<float>(0.0f) };

			if (entry.File.empty())
			{
				WLD_CORE_WARN("ChunkIndex: 跳过没有 File 的条目(Id={0})", entry.ChunkID);
				continue;
			}
			parsed.m_Entries.push_back(std::move(entry));
		}

		*out = std::move(parsed);
		return true;
	}

	bool ChunkIndex::LoadFromFile(const std::filesystem::path& path, ChunkIndex* out, std::string* error)
	{
		if (!std::filesystem::is_regular_file(path))
		{
			if (error) *error = "chunk index not found: " + path.u8string();
			return false;
		}
		try
		{
			std::ifstream fin(path, std::ios::binary);
			if (!fin.is_open())
			{
				if (error) *error = "cannot open chunk index: " + path.u8string();
				return false;
			}
			std::string text((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
			return Parse(text, out, error);
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	bool ChunkIndex::Save(const std::filesystem::path& path, const ChunkIndex& index, std::string* error)
	{
		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "Chunks" << YAML::Value << YAML::BeginSeq;
			for (const ChunkIndexEntry& entry : index.m_Entries)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "Id" << YAML::Value << entry.ChunkID;
				out << YAML::Key << "File" << YAML::Value << entry.File;
				out << YAML::Key << "Entities" << YAML::Value << entry.EntityCount;
				out << YAML::Key << "DataLayerMask" << YAML::Value << entry.DataLayerMask;
				out << YAML::Key << "Min" << YAML::Value << YAML::Flow << YAML::BeginSeq
					<< entry.Bounds.Min.x << entry.Bounds.Min.y << entry.Bounds.Min.z << YAML::EndSeq;
				out << YAML::Key << "Max" << YAML::Value << YAML::Flow << YAML::BeginSeq
					<< entry.Bounds.Max.x << entry.Bounds.Max.y << entry.Bounds.Max.z << YAML::EndSeq;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;

			if (path.has_parent_path())
				std::filesystem::create_directories(path.parent_path());

			std::ofstream fout(path);
			if (!fout.is_open())
			{
				if (error) *error = "cannot open chunk index for write: " + path.u8string();
				return false;
			}
			fout << out.c_str();
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}
}
