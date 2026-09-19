#include "wldpch.h"
#include "World/Core/Asset/ModelImportSettings.h"

#include "World/WUI/WuiJson.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace World::Asset
{
	namespace
	{
		constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
		constexpr uint64_t kFnvPrime = 1099511628211ULL;

		uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= kFnvPrime;
			}
			return hash;
		}

		uint64_t HashF32(uint64_t hash, float value)
		{
			uint32_t bits = 0;
			static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
			std::memcpy(&bits, &value, sizeof(bits));
			return HashBytes(hash, &bits, sizeof(bits));
		}

		std::filesystem::path SettingsPath(const std::string& sourcePath)
		{
			std::filesystem::path path(sourcePath);
			path.replace_extension(".wimport");
			return path;
		}

		bool ReadText(const std::filesystem::path& path, std::string& out)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return false;
			out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
			return true;
		}

		void Warn(std::string* reason, const std::string& message)
		{
			if (reason)
			{
				if (!reason->empty())
					*reason += "; ";
				*reason += message;
			}
		}
	}

	ModelImportSettings ModelImportSettings::Load(const std::string& sourcePath, std::string* reason)
	{
		if (reason)
			reason->clear();
		ModelImportSettings settings = Default();
		if (sourcePath.empty())
		{
			Warn(reason, "source path is empty; using default import settings");
			return settings;
		}

		const std::filesystem::path path = SettingsPath(sourcePath);
		std::error_code existsEc;
		if (!std::filesystem::is_regular_file(path, existsEc))
			return settings;   // 缺文件 = 默认值,正常情况,不报错

		std::string text;
		if (!ReadText(path, text) || text.empty())
		{
			Warn(reason, "cannot read '" + path.string() + "'; using default import settings");
			return settings;
		}
		std::string parseError;
		const std::optional<Wui::JsonValue> root = Wui::JsonValue::Parse(text, &parseError);
		if (!root || root->type != Wui::JsonValue::Type::Object)
		{
			Warn(reason, "invalid JSON in '" + path.string() + "'"
				+ (parseError.empty() ? std::string() : " (" + parseError + ")")
				+ "; using default import settings");
			return settings;
		}

		if (const Wui::JsonValue* node = root->Find("scale"))
		{
			if (node->type == Wui::JsonValue::Type::Number)
			{
				const double value = node->AsNumber(1.0);
				if (std::isfinite(value) && value > 0.0)
					settings.Scale = static_cast<float>(value);
				else
					Warn(reason, "import settings '" + path.string()
						+ "': scale must be a positive finite number; using 1");
			}
			else
				Warn(reason, "import settings '" + path.string() + "': scale is not a number; using 1");
		}
		if (const Wui::JsonValue* node = root->Find("upAxis"))
		{
			if (node->type == Wui::JsonValue::Type::String)
			{
				const std::string axis = node->AsString();
				if (axis == "Z")
					settings.UpAxis = 1;
				else if (axis == "Y")
					settings.UpAxis = 0;
				else
					Warn(reason, "import settings '" + path.string()
						+ "': upAxis must be \"Y\" or \"Z\"; using Y");
			}
			else
				Warn(reason, "import settings '" + path.string() + "': upAxis is not a string; using Y");
		}
		if (const Wui::JsonValue* node = root->Find("exportMaterials"))
		{
			if (node->type == Wui::JsonValue::Type::Bool)
				settings.ExportMaterials = node->Bool;
			else
				Warn(reason, "import settings '" + path.string()
					+ "': exportMaterials is not a bool; using true");
		}
		if (const Wui::JsonValue* node = root->Find("exportTextures"))
		{
			if (node->type == Wui::JsonValue::Type::Bool)
				settings.ExportTextures = node->Bool;
			else
				Warn(reason, "import settings '" + path.string()
					+ "': exportTextures is not a bool; using true");
		}
		if (const Wui::JsonValue* node = root->Find("importAnimations"))
		{
			if (node->type == Wui::JsonValue::Type::Bool)
				settings.ImportAnimations = node->Bool;
			else
				Warn(reason, "import settings '" + path.string()
					+ "': importAnimations is not a bool; using true");
		}
		if (const Wui::JsonValue* node = root->Find("importSkins"))
		{
			if (node->type == Wui::JsonValue::Type::Bool)
				settings.ImportSkins = node->Bool;
			else
				Warn(reason, "import settings '" + path.string()
					+ "': importSkins is not a bool; using true");
		}
		if (const Wui::JsonValue* node = root->Find("animationSampleRate"))
		{
			if (node->type == Wui::JsonValue::Type::Number)
			{
				const double value = node->AsNumber(30.0);
				if (std::isfinite(value))
				{
					// D5c-2:采样率 clamp 到 1..120(0/负数/过大都钳制,不失败)。
					const double clamped = std::min(120.0, std::max(1.0, value));
					if (clamped != value)
						Warn(reason, "import settings '" + path.string()
							+ "': animationSampleRate " + std::to_string(value)
							+ " was clamped to " + std::to_string(clamped));
					settings.AnimationSampleRate = static_cast<float>(clamped);
				}
				else
					Warn(reason, "import settings '" + path.string()
						+ "': animationSampleRate must be a finite number; using 30");
			}
			else
				Warn(reason, "import settings '" + path.string()
					+ "': animationSampleRate is not a number; using 30");
		}
		if (const Wui::JsonValue* node = root->Find("generateNormals"))
		{
			if (node->type == Wui::JsonValue::Type::Bool)
				settings.GenerateNormals = node->Bool;
			else
				Warn(reason, "import settings '" + path.string()
					+ "': generateNormals is not a bool; using true");
		}
		if (const Wui::JsonValue* node = root->Find("reuseMaterials"))
		{
			if (node->type == Wui::JsonValue::Type::Bool)
				settings.ReuseMaterials = node->Bool;
			else
				Warn(reason, "import settings '" + path.string()
					+ "': reuseMaterials is not a bool; using true");
		}
		if (const Wui::JsonValue* node = root->Find("reuseTextures"))
		{
			if (node->type == Wui::JsonValue::Type::Bool)
				settings.ReuseTextures = node->Bool;
			else
				Warn(reason, "import settings '" + path.string()
					+ "': reuseTextures is not a bool; using true");
		}
		if (const Wui::JsonValue* node = root->Find("sharedMaterialFolder"))
		{
			if (node->type == Wui::JsonValue::Type::String)
				settings.SharedMaterialFolder = node->String;
			else
				Warn(reason, "import settings '" + path.string()
					+ "': sharedMaterialFolder is not a string; using empty");
		}
		return settings;
	}

	bool ModelImportSettings::Save(const std::string& sourcePath, const ModelImportSettings& settings,
		std::string* reason)
	{
		if (reason)
			reason->clear();
		if (sourcePath.empty())
		{
			Warn(reason, "source path is empty");
			return false;
		}
		const std::filesystem::path path = SettingsPath(sourcePath);

		Wui::JsonValue root;
		root.type = Wui::JsonValue::Type::Object;
		root.Object.push_back({ "scale", Wui::JsonValue::MakeNumber(settings.Scale) });
		root.Object.push_back({ "upAxis",
			Wui::JsonValue::MakeString(settings.UpAxis == 0 ? "Y" : "Z") });
		root.Object.push_back({ "exportMaterials", Wui::JsonValue::MakeBool(settings.ExportMaterials) });
		root.Object.push_back({ "exportTextures", Wui::JsonValue::MakeBool(settings.ExportTextures) });
		root.Object.push_back({ "importAnimations", Wui::JsonValue::MakeBool(settings.ImportAnimations) });
		root.Object.push_back({ "importSkins", Wui::JsonValue::MakeBool(settings.ImportSkins) });
		root.Object.push_back({ "animationSampleRate",
			Wui::JsonValue::MakeNumber(settings.AnimationSampleRate) });
		root.Object.push_back({ "generateNormals", Wui::JsonValue::MakeBool(settings.GenerateNormals) });
		root.Object.push_back({ "reuseMaterials", Wui::JsonValue::MakeBool(settings.ReuseMaterials) });
		root.Object.push_back({ "reuseTextures", Wui::JsonValue::MakeBool(settings.ReuseTextures) });
		root.Object.push_back({ "sharedMaterialFolder", Wui::JsonValue::MakeString(settings.SharedMaterialFolder) });

		std::error_code ec;
		if (!path.parent_path().empty())
			std::filesystem::create_directories(path.parent_path(), ec);
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			Warn(reason, "cannot write '" + path.string() + "'");
			return false;
		}
		stream << root.Dump();
		if (!stream)
		{
			Warn(reason, "write failed: '" + path.string() + "'");
			return false;
		}
		return true;
	}

	uint64_t ModelImportSettings::Hash(const ModelImportSettings& settings)
	{
		// 字段顺序固定;新增字段追加在末尾(旧值不会因字段顺序变化而改变)。
		uint64_t hash = kFnvOffset;
		hash = HashF32(hash, settings.Scale);
		hash = HashBytes(hash, &settings.UpAxis, sizeof(settings.UpAxis));
		const uint8_t flags =
			(settings.ExportMaterials ? 1u : 0u)
			| (settings.ExportTextures ? 2u : 0u)
			| (settings.ImportAnimations ? 4u : 0u)
			| (settings.GenerateNormals ? 8u : 0u);
		hash = HashBytes(hash, &flags, sizeof(flags));
		// D5c-2:skin 开关与动画采样率也参与哈希(改了 → 需要重导)。
		const uint8_t skinFlags = settings.ImportSkins ? 1u : 0u;
		hash = HashBytes(hash, &skinFlags, sizeof(skinFlags));
		hash = HashF32(hash, settings.AnimationSampleRate);
		// D10:复用开关与共享目录也参与哈希(改了 → 需要重导)。
		const uint8_t reuseFlags =
			(settings.ReuseMaterials ? 1u : 0u)
			| (settings.ReuseTextures ? 2u : 0u);
		hash = HashBytes(hash, &reuseFlags, sizeof(reuseFlags));
		hash = HashBytes(hash, settings.SharedMaterialFolder.data(), settings.SharedMaterialFolder.size());
		return hash;
	}
}
