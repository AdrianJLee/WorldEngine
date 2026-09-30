#include "wldpch.h"
#include "World/Plugins/PluginManifest.h"

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <stdexcept>
#include <string_view>

namespace World::Plugins
{
	namespace
	{
		// ---- YAML 字段读取(写法照 World/Core/Asset/ProjectManifest.cpp)------------

		bool ReadString(const YAML::Node& node, const char* key, std::string* out, std::string* error)
		{
			if (!node[key])
				return true;
			try
			{
				*out = node[key].as<std::string>("");
				return true;
			}
			catch (const std::exception&)
			{
				if (error) *error = std::string("field '") + key + "' must be a string";
				return false;
			}
		}

		bool ReadU32(const YAML::Node& node, const char* key, uint32_t* out, std::string* error)
		{
			if (!node[key])
				return true;
			try
			{
				*out = node[key].as<uint32_t>(*out);
				return true;
			}
			catch (const std::exception&)
			{
				if (error) *error = std::string("field '") + key + "' must be an unsigned integer";
				return false;
			}
		}

		bool ReadStringList(const YAML::Node& node, const char* key, std::vector<std::string>* out, std::string* error)
		{
			const YAML::Node list = node[key];
			if (!list)
				return true;
			if (!list.IsSequence())
			{
				if (error) *error = std::string("field '") + key + "' must be a list of strings";
				return false;
			}
			for (const YAML::Node& item : list)
			{
				std::string value;
				try
				{
					value = item.as<std::string>("");
				}
				catch (const std::exception&)
				{
					if (error) *error = std::string("field '") + key + "' must be a list of strings";
					return false;
				}
				if (value.empty())
				{
					if (error) *error = std::string("field '") + key + "' must not contain empty entries";
					return false;
				}
				out->push_back(std::move(value));
			}
			return true;
		}

		bool ReadNumber(std::string_view text, uint32_t* out)
		{
			if (text.empty())
				return false;
			uint32_t value = 0;
			const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
			if (result.ec != std::errc() || result.ptr != text.data() + text.size())
				return false;
			*out = value;
			return true;
		}

		// engine: 只接受空或 ">=X.Y"(方案 §4.1);其它形态干净拒绝,不猜语义。
		bool ParseEngineConstraint(const std::string& text, uint32_t* major, uint32_t* minor, std::string* error)
		{
			*major = 0;
			*minor = 0;
			if (text.empty())
				return true;
			const std::string_view view(text);
			const size_t dot = view.find('.', 2);
			if (view.size() < 4 || view.substr(0, 2) != ">=" || dot == std::string_view::npos
				|| !ReadNumber(view.substr(2, dot - 2), major)
				|| !ReadNumber(view.substr(dot + 1), minor))
			{
				if (error) *error = "field 'engine' must look like '>=X.Y' (or be empty): " + text;
				return false;
			}
			return true;
		}

		bool ReadShip(const YAML::Node& root, PluginShipPolicy* out, std::string* error)
		{
			const YAML::Node node = root["ship"];
			if (!node)
				return true;
			std::string text;
			try
			{
				text = node.as<std::string>("");
			}
			catch (const std::exception&)
			{
				if (error) *error = "field 'ship' must be a string";
				return false;
			}
			if (text == "auto")
				*out = PluginShipPolicy::Auto;
			else if (text == "always")
				*out = PluginShipPolicy::Always;
			else if (text == "never")
				*out = PluginShipPolicy::Never;
			else
			{
				if (error) *error = "field 'ship' must be auto|always|never: " + text;
				return false;
			}
			return true;
		}

		// 位置即 scope:写了就必须与位置一致(方案 §1/§8)。
		bool ReadScope(const YAML::Node& root, PluginScope location, PluginScope* out, bool* present,
			std::string* error)
		{
			const YAML::Node node = root["scope"];
			if (!node)
				return true;
			*present = true;
			std::string text;
			try
			{
				text = node.as<std::string>("");
			}
			catch (const std::exception&)
			{
				if (error) *error = "field 'scope' must be a string";
				return false;
			}
			if (text == "engine")
				*out = PluginScope::Engine;
			else if (text == "project")
				*out = PluginScope::Project;
			else
			{
				if (error) *error = "field 'scope' must be engine|project: " + text;
				return false;
			}
			if (*out != location)
			{
				if (error) *error = "manifest scope '" + text + "' does not match its location ('"
					+ PluginScopeName(location) + "')";
				return false;
			}
			return true;
		}
	}

	bool PluginManifest::Load(const std::filesystem::path& manifestPath, PluginScope locationScope,
		PluginManifest* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "null output manifest";
			return false;
		}

		PluginManifest manifest;
		manifest.Scope = locationScope;
		manifest.ManifestPath = manifestPath;
		manifest.Root = manifestPath.parent_path();

		const auto fail = [&](const std::string& reason)
		{
			if (error) *error = "plugin manifest '" + manifestPath.string() + "': " + reason;
			*out = manifest;   // 已解析出的字段保留(诊断仍能显示 id)
			return false;
		};

		try
		{
			const YAML::Node root = YAML::LoadFile(manifestPath.string());
			if (!root || !root.IsMap())
				return fail("root must be a map");

			std::string reason;
			if (!ReadString(root, "id", &manifest.Id, &reason)
				|| !ReadString(root, "name", &manifest.Name, &reason)
				|| !ReadString(root, "version", &manifest.Version, &reason)
				|| !ReadString(root, "publisher", &manifest.Publisher, &reason)
				|| !ReadString(root, "signature", &manifest.Signature, &reason)
				|| !ReadU32(root, "abi", &manifest.Abi, &reason)
				|| !ReadString(root, "entry", &manifest.Entry, &reason)
				|| !ReadString(root, "engine", &manifest.Engine, &reason)
				|| !ReadStringList(root, "depends", &manifest.Depends, &reason)
				|| !ReadStringList(root, "provides", &manifest.Provides, &reason)
				|| !ReadStringList(root, "overrides", &manifest.Overrides, &reason)
				|| !ReadShip(root, &manifest.Ship, &reason)
				|| !ReadScope(root, locationScope, &manifest.Scope, &manifest.HasScopeField, &reason))
				return fail(reason);

			if (manifest.Id.empty())
				return fail("field 'id' must not be empty");
			if (manifest.Name.empty())
				manifest.Name = manifest.Id;
			if (manifest.Entry.empty())
				return fail("field 'entry' must not be empty");
			if (manifest.Abi != WE_PLUGIN_ABI_VERSION)
				return fail("manifest 'abi' (" + std::to_string(manifest.Abi)
					+ ") does not match the host plugin ABI (" + std::to_string(WE_PLUGIN_ABI_VERSION) + ")");
			if (!ParseEngineConstraint(manifest.Engine, &manifest.EngineMinMajor, &manifest.EngineMinMinor, &reason))
				return fail(reason);

			// 产物定位(方案 §4.1):<插件目录>/bin/<目录名><扩展名>。
			manifest.LibraryPath = manifest.Root / "bin"
				/ (manifest.Root.filename().string() + kPluginLibraryExtension);

			*out = std::move(manifest);
			return true;
		}
		catch (const std::exception& exception)
		{
			return fail(std::string("cannot read manifest: ") + exception.what());
		}
	}
}
