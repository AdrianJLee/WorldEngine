#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace World
{
	enum class AssetCategory : uint8_t
	{
		Unknown = 0,
		Texture = 1,
		Material = 2,
		Mesh = 3,
		Prefab = 4,
		Script = 5,
		Audio = 6,
		Level = 7
	};

	struct ThumbnailInfo
	{
		std::string Path;
		AssetCategory Category = AssetCategory::Unknown;
		uint64_t TextureId = 0;
		uint32_t Width = 64;
		uint32_t Height = 64;
		bool IsReady = false;
	};

	class WLD_API AssetThumbnailCache
	{
	public:
		static AssetThumbnailCache& Get();

		static AssetCategory DetectCategory(std::string_view path);

		void RegisterThumbnail(const std::string& path, const ThumbnailInfo& info);
		bool HasThumbnail(const std::string& path) const;
		const ThumbnailInfo* GetThumbnail(const std::string& path) const;
		ThumbnailInfo& GetOrCreateThumbnail(const std::string& path);
		void Clear();
		size_t Count() const;

	private:
		AssetThumbnailCache() = default;
		~AssetThumbnailCache() = default;

		AssetThumbnailCache(const AssetThumbnailCache&) = delete;
		AssetThumbnailCache& operator=(const AssetThumbnailCache&) = delete;

		std::unordered_map<std::string, ThumbnailInfo> m_Thumbnails;
	};
}
