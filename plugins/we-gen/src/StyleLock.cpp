#include "StyleLock.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Gen
{
	namespace fs = std::filesystem;

	std::string StyleLock::Apply(const std::string& userPrompt) const
	{
		if (promptTemplate.empty())
			return userPrompt;

		const size_t pos = promptTemplate.find("{prompt}");
		if (pos != std::string::npos)
		{
			std::string res = promptTemplate;
			res.replace(pos, 8, userPrompt);
			return res;
		}

		return userPrompt + ", " + promptTemplate;
	}

	std::string StyleLock::ToYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "prompt_template" << YAML::Value << promptTemplate;
		out << YAML::Key << "negative_prompt" << YAML::Value << negativePrompt;
		out << YAML::Key << "palette" << YAML::Value << YAML::BeginSeq;
		for (const auto& c : palette) out << c;
		out << YAML::EndSeq;
		out << YAML::Key << "seed_min" << YAML::Value << seedRangeMin;
		out << YAML::Key << "seed_max" << YAML::Value << seedRangeMax;
		out << YAML::Key << "reference_hash" << YAML::Value << referenceHash;
		out << YAML::EndMap;
		return out.c_str();
	}

	StyleLock StyleLock::FromYaml(const std::string& yamlStr)
	{
		StyleLock lock;
		try
		{
			YAML::Node doc = YAML::Load(yamlStr);
			if (doc["prompt_template"]) lock.promptTemplate = doc["prompt_template"].as<std::string>();
			if (doc["negative_prompt"]) lock.negativePrompt = doc["negative_prompt"].as<std::string>();
			if (doc["palette"] && doc["palette"].IsSequence())
			{
				lock.palette.clear();
				for (const auto& c : doc["palette"]) lock.palette.push_back(c.as<std::string>());
			}
			if (doc["seed_min"]) lock.seedRangeMin = doc["seed_min"].as<int>();
			if (doc["seed_max"]) lock.seedRangeMax = doc["seed_max"].as<int>();
			if (doc["reference_hash"]) lock.referenceHash = doc["reference_hash"].as<std::string>();
		}
		catch (...) {}
		return lock;
	}

	StyleLock StyleLock::Load(const std::string& projectRoot)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path lockPath = root / "art" / "style-lock.yaml";

		if (fs::exists(lockPath))
		{
			std::ifstream f(lockPath);
			std::string str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			return FromYaml(str);
		}
		return StyleLock {};
	}

	bool StyleLock::Save(const std::string& projectRoot, const StyleLock& lock)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path artDir = root / "art";

		std::error_code ec;
		fs::create_directories(artDir, ec);

		std::ofstream f(artDir / "style-lock.yaml");
		if (f)
		{
			f << lock.ToYaml();
			return true;
		}
		return false;
	}
}
