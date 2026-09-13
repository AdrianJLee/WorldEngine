#include "wldpch.h"
#include "World/Core/Asset/ProjectManifest.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>

namespace World::Asset
{
	namespace
	{
		// 相对路径校验:非空、非绝对、无盘符、无 ".." 段,统一用 '/'。
		bool ValidateRelativePath(const std::string& raw, std::string* normalized, std::string* error)
		{
			if (raw.empty())
			{
				if (error) *error = "path must not be empty";
				return false;
			}
			if (raw.find('\\') != std::string::npos)
			{
				if (error) *error = "path must use '/' separators: " + raw;
				return false;
			}
			const std::filesystem::path path(raw);
			if (path.is_absolute() || path.has_root_name())
			{
				if (error) *error = "path must be relative: " + raw;
				return false;
			}
			for (const auto& part : path)
				if (part == "..")
				{
					if (error) *error = "path must not contain '..': " + raw;
					return false;
				}
			std::string cleaned;
			for (const auto& part : path)
			{
				if (!cleaned.empty() && cleaned.back() != '/')
					cleaned += '/';
				cleaned += part.string();
			}
			if (normalized)
				*normalized = cleaned.empty() ? raw : cleaned;
			return true;
		}

		bool ValidateManifest(ProjectManifest& manifest, std::string* error)
		{
			if (manifest.Id.empty())
			{
				if (error) *error = "manifest 'id' must not be empty";
				return false;
			}
			std::string contentRoot;
			if (!ValidateRelativePath(manifest.ContentRoot.generic_string(), &contentRoot, error))
				return false;
			manifest.ContentRoot = contentRoot;
			if (manifest.StartScene.empty() ||
				!ValidateRelativePath(manifest.StartScene, &manifest.StartScene, error))
				return false;
			if (manifest.Renderer != "opengl" && manifest.Renderer != "vulkan")
			{
				if (error) *error = "renderer must be 'opengl' or 'vulkan': " + manifest.Renderer;
				return false;
			}
			for (std::string& package : manifest.Packages)
				if (!ValidateRelativePath(package, &package, error))
					return false;
			return true;
		}
	}

	bool ProjectManifest::Load(const std::filesystem::path& path, ProjectManifest* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "null output manifest";
			return false;
		}
		try
		{
			const YAML::Node root = YAML::LoadFile(path.string());
			if (!root || !root.IsMap())
			{
				if (error) *error = "manifest root must be a map: " + path.string();
				return false;
			}
			ProjectManifest manifest;
			manifest.Id = root["id"] ? root["id"].as<std::string>("") : "";
			manifest.Version = root["version"] ? root["version"].as<std::string>("1.0.0") : "1.0.0";
			manifest.ContentRoot = root["content_root"] ? root["content_root"].as<std::string>("assets") : "assets";
			manifest.StartScene = root["start_scene"] ? root["start_scene"].as<std::string>("") : "";
			manifest.Renderer = root["renderer"] ? root["renderer"].as<std::string>("opengl") : "opengl";
			if (const YAML::Node packages = root["packages"])
				for (const YAML::Node& item : packages)
					manifest.Packages.push_back(item.as<std::string>(""));
			if (!ValidateManifest(manifest, error))
				return false;
			*out = std::move(manifest);
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error) *error = exception.what();
			return false;
		}
	}

	bool ProjectManifest::Save(const std::filesystem::path& path, const ProjectManifest& manifest, std::string* error)
	{
		ProjectManifest copy = manifest;
		if (!ValidateManifest(copy, error))
			return false;
		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "id" << YAML::Value << copy.Id;
			out << YAML::Key << "version" << YAML::Value << copy.Version;
			out << YAML::Key << "content_root" << YAML::Value << copy.ContentRoot.generic_string();
			out << YAML::Key << "start_scene" << YAML::Value << copy.StartScene;
			out << YAML::Key << "renderer" << YAML::Value << copy.Renderer;
			out << YAML::Key << "packages" << YAML::Value << YAML::BeginSeq;
			for (const std::string& package : copy.Packages)
				out << package;
			out << YAML::EndSeq;
			out << YAML::EndMap;

			if (!path.parent_path().empty())
			{
				std::error_code ec;
				std::filesystem::create_directories(path.parent_path(), ec);
				if (ec)
				{
					if (error) *error = ec.message();
					return false;
				}
			}
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				if (error) *error = "cannot open manifest for writing: " + path.string();
				return false;
			}
			stream << out.c_str();
			return static_cast<bool>(stream);
		}
		catch (const std::exception& exception)
		{
			if (error) *error = exception.what();
			return false;
		}
	}

	std::filesystem::path ProjectManifest::ResolveContentRoot(const std::filesystem::path& manifestPath) const
	{
		const std::filesystem::path base = std::filesystem::absolute(manifestPath).parent_path();
		return (base / ContentRoot).lexically_normal();
	}

	bool ProjectManifest::Locate(const std::filesystem::path& workingDirectory, std::filesystem::path* manifestPath)
	{
		std::error_code ec;
		for (const char* candidate : { "project.we.yaml", "Game/project.we.yaml" })
		{
			const std::filesystem::path full = workingDirectory / candidate;
			if (std::filesystem::is_regular_file(full, ec))
			{
				if (manifestPath)
					*manifestPath = full;
				return true;
			}
		}
		return false;
	}
}
