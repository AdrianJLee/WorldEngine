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
			// rendering 区块(可缺省):数值范围与 2 的幂约束在这里统一拒绝,
			// 避免"贴图尺寸 100"这类值到初始化期才炸在 GPU 资源创建里。
			const RenderingSettings& rendering = manifest.Rendering;
			const uint32_t shadowSize = rendering.ShadowMapSize;
			if (shadowSize < 256 || shadowSize > 4096 || (shadowSize & (shadowSize - 1)) != 0)
			{
				if (error) *error = "rendering.shadow_map_size must be a power of two in [256, 4096]: "
					+ std::to_string(shadowSize);
				return false;
			}
			if (rendering.MaxDirectionalLights < 1 || rendering.MaxDirectionalLights > 2)
			{
				if (error) *error = "rendering.max_directional_lights must be in [1, 2]: "
					+ std::to_string(rendering.MaxDirectionalLights);
				return false;
			}
			if (rendering.MaxPointLights > 7)
			{
				if (error) *error = "rendering.max_point_lights must be in [0, 7]: "
					+ std::to_string(rendering.MaxPointLights);
				return false;
			}
			if (rendering.MaxDirectionalLights + rendering.MaxPointLights > 8)
			{
				if (error) *error = "rendering light limits exceed the 8-light UBO capacity";
				return false;
			}
			// P4-1:各向异性上限(1..16)——超过设备能力时引擎内部再退化。
			if (rendering.Anisotropy < 1 || rendering.Anisotropy > 16)
			{
				if (error) *error = "rendering.anisotropy must be in [1, 16]: "
					+ std::to_string(rendering.Anisotropy);
				return false;
			}
			// P4-1:物理设置范围校验。
			if (manifest.Physics.FixedStepHz < 1 || manifest.Physics.FixedStepHz > 240)
			{
				if (error) *error = "physics.fixed_step_hz must be in [1, 240]: "
					+ std::to_string(manifest.Physics.FixedStepHz);
				return false;
			}
			if (!std::isfinite(manifest.Physics.Gravity))
			{
				if (error) *error = "physics.gravity must be a finite number";
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
			if (const YAML::Node rendering = root["rendering"])
			{
				if (!rendering.IsMap())
				{
					if (error) *error = "manifest 'rendering' must be a map: " + path.string();
					return false;
				}
				auto readBool = [&rendering](const char* key, bool fallback)
				{
					return rendering[key] ? rendering[key].as<bool>(fallback) : fallback;
				};
				auto readU32 = [&rendering](const char* key, uint32_t fallback)
				{
					return rendering[key] ? rendering[key].as<uint32_t>(fallback) : fallback;
				};
				manifest.Rendering.Culling = readBool("culling", manifest.Rendering.Culling);
				manifest.Rendering.Shadows = readBool("shadows", manifest.Rendering.Shadows);
				manifest.Rendering.ShadowMapSize = readU32("shadow_map_size", manifest.Rendering.ShadowMapSize);
				manifest.Rendering.MaxDirectionalLights =
					readU32("max_directional_lights", manifest.Rendering.MaxDirectionalLights);
				manifest.Rendering.MaxPointLights =
					readU32("max_point_lights", manifest.Rendering.MaxPointLights);
				manifest.Rendering.GpuTiming = readBool("gpu_timing", manifest.Rendering.GpuTiming);
				manifest.Rendering.Instancing = readBool("instancing", manifest.Rendering.Instancing);
				manifest.Rendering.Anisotropy = readU32("anisotropy", manifest.Rendering.Anisotropy);
			}
			if (const YAML::Node physics = root["physics"])
			{
				if (!physics.IsMap())
				{
					if (error) *error = "manifest 'physics' must be a map: " + path.string();
					return false;
				}
				if (physics["fixed_step_hz"])
					manifest.Physics.FixedStepHz = physics["fixed_step_hz"].as<uint32_t>(manifest.Physics.FixedStepHz);
				if (physics["gravity"])
					manifest.Physics.Gravity = physics["gravity"].as<float>(manifest.Physics.Gravity);
			}
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
			out << YAML::Key << "rendering" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "culling" << YAML::Value << copy.Rendering.Culling;
			out << YAML::Key << "shadows" << YAML::Value << copy.Rendering.Shadows;
			out << YAML::Key << "shadow_map_size" << YAML::Value << copy.Rendering.ShadowMapSize;
			out << YAML::Key << "max_directional_lights" << YAML::Value << copy.Rendering.MaxDirectionalLights;
			out << YAML::Key << "max_point_lights" << YAML::Value << copy.Rendering.MaxPointLights;
			out << YAML::Key << "gpu_timing" << YAML::Value << copy.Rendering.GpuTiming;
			out << YAML::Key << "instancing" << YAML::Value << copy.Rendering.Instancing;
			out << YAML::Key << "anisotropy" << YAML::Value << copy.Rendering.Anisotropy;
			out << YAML::EndMap;
			out << YAML::Key << "physics" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "fixed_step_hz" << YAML::Value << copy.Physics.FixedStepHz;
			out << YAML::Key << "gravity" << YAML::Value << copy.Physics.Gravity;
			out << YAML::EndMap;
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
			stream << '\n';   // YAML 文件保持行尾换行(git 差异干净)
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
