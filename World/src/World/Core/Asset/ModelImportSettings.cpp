#include "wldpch.h"
#include "World/Core/Asset/ModelImportSettings.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Asset/WModelIO.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

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

	// P4-U12:唯一解析入口 —— 已有 .wmodel 的 meta(资产自描述)> 项目默认 > 引擎默认。
	ModelImportSettings ModelImportSettings::ResolveForImport(const std::string& existingModelPath,
		std::string* reason, bool* fromAsset)
	{
		if (reason)
			reason->clear();
		if (fromAsset)
			*fromAsset = false;
		if (!existingModelPath.empty())
		{
			WModelData::MetaData meta;
			std::string metaError;
			if (!WModelIO::ReadMeta(existingModelPath, meta, &metaError))
			{
				if (reason)
					*reason = "cannot read import settings from '" + existingModelPath + "'"
						+ (metaError.empty() ? std::string() : " (" + metaError + ")")
						+ "; using project defaults";
			}
			else if (meta.HasSettings)
			{
				if (fromAsset)
					*fromAsset = true;
				return meta.Settings;
			}
		}
		return ProjectDefaults();
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
