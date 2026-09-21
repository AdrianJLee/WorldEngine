#include "wldpch.h"
#include "World/Core/Asset/ModelImportSettings.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Asset/WModelIO.h"
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

		// 小写扩展名(含点);FindProducedModel 用。
		std::string LowerExtensionOf(const std::filesystem::path& path)
		{
			std::string extension = path.extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return extension;
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
		// P4-U4:没有 `.wimport` 时用**项目级导入默认**(project.we.yaml 的 imports:);
		// 未设置时它就是 Default(),与旧行为逐字节一致。
		ModelImportSettings settings = ProjectDefaults();
		if (sourcePath.empty())
		{
			Warn(reason, "source path is empty; using default import settings");
			return settings;
		}

		const std::filesystem::path path = SettingsPath(sourcePath);
		std::error_code existsEc;
		if (!std::filesystem::is_regular_file(path, existsEc))
			return settings;   // 缺文件 = 项目默认值(未设置时=引擎默认),正常情况,不报错

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

	// ---- P4-U4:项目级导入默认值 ----
	// 进程内缓存(与 PhysicsSettings 同款):清单是事实源,这里只是"当前生效值"。
	// 用函数内 static 而不是全局对象:避免静态初始化顺序问题(DLL 里尤其明显)。
	namespace
	{
		ModelImportSettings& ProjectDefaultsStorage()
		{
			static ModelImportSettings defaults;
			return defaults;
		}
	}

	void ModelImportSettings::SetProjectDefaults(const ModelImportSettings& settings)
	{
		ProjectDefaultsStorage() = settings;
	}

	const ModelImportSettings& ModelImportSettings::ProjectDefaults()
	{
		return ProjectDefaultsStorage();
	}

	bool ModelImportSettings::LoadProjectDefaults(const std::string& manifestPath)
	{
		// 循环包含规避:ProjectManifest.h 里放了 ModelImportSettings 字段,这里只在 .cpp 里用它。
		std::string error;
		ProjectManifest manifest;
		if (manifestPath.empty() || !ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			WLD_CORE_WARN("[import] 读取项目导入默认值失败({0});回到引擎默认",
				error.empty() ? std::string("manifest path is empty") : error);
			SetProjectDefaults(Default());
			return false;
		}
		SetProjectDefaults(manifest.ImportDefaults);
		return true;
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


	// P4-U11:唯一设置解析入口 —— 已有 .wmodel 的 meta(资产自描述)> 旧 .wimport >
	// project.we.yaml imports: > 引擎默认。
	ModelImportSettings ModelImportSettings::ResolveForImport(const std::string& sourcePath,
		const std::string& existingModelPath, std::string* reason, bool* fromAsset)
	{
		if (fromAsset)
			*fromAsset = false;
		if (!existingModelPath.empty())
		{
			WModelData::MetaData meta;
			std::string metaError;
			if (WModelIO::ReadMeta(existingModelPath, meta, &metaError) && meta.HasSettings)
			{
				if (fromAsset)
					*fromAsset = true;
				if (reason)
					reason->clear();
				return meta.Settings;
			}
		}
		// 旧项目:旁路 .wimport(缺失/坏 JSON → 项目默认 + warning)。
		return Load(sourcePath, reason);
	}


	// P4-U11:按 meta.SourcePath 找产物(不信文件名 —— 用户可能把源与产物放在不同目录,
// 也可能同一目录里有多个模型)。同目录里的 .wmodel 通常只有个位数,逐个小读 meta 足够便宜。
	std::string ModelImportSettings::FindProducedModel(const std::string& sourceDirectory,
		const std::string& sourceLogicalPath)
	{
		if (sourceDirectory.empty() || sourceLogicalPath.empty())
			return std::string();
		std::error_code ec;
		const std::filesystem::path directory(sourceDirectory);
		if (!std::filesystem::is_directory(directory, ec))
			return std::string();
		std::string normalizedSource = sourceLogicalPath;
		std::replace(normalizedSource.begin(), normalizedSource.end(), '\\', '/');
		for (const std::filesystem::directory_entry& entry :
			std::filesystem::directory_iterator(directory, std::filesystem::directory_options::skip_permission_denied, ec))
		{
			if (!entry.is_regular_file(ec) || LowerExtensionOf(entry.path()) != ".wmodel")
				continue;
			WModelData::MetaData meta;
			std::string metaError;
			if (!WModelIO::ReadMeta(entry.path().string(), meta, &metaError) || !meta.Valid)
				continue;
			std::string candidate = meta.SourcePath;
			std::replace(candidate.begin(), candidate.end(), '\\', '/');
			if (candidate == normalizedSource)
				return entry.path().string();
			// 调用方只给了文件名(拿不到逻辑路径)时按"同目录 + 以 /<文件名> 结尾"匹配:
			// 产物与源同目录,这一条足以认出来(P4-U11 的 SettingsFingerprint 就是这种调用)。
			if (candidate.size() > normalizedSource.size()
				&& candidate.compare(candidate.size() - normalizedSource.size(), normalizedSource.size(), normalizedSource) == 0
				&& candidate[candidate.size() - normalizedSource.size() - 1] == '/')
				return entry.path().string();
		}
		return std::string();
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
