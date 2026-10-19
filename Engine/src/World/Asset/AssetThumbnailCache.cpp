#include "wldpch.h"
#include "World/Asset/AssetThumbnailCache.h"
#include "World/WUI/WuiContext.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace World
{
	AssetThumbnailCache& AssetThumbnailCache::Get()
	{
		static AssetThumbnailCache s_Instance;
		return s_Instance;
	}

	AssetCategory AssetThumbnailCache::DetectCategory(std::string_view path)
	{
		std::filesystem::path fsPath(path);
		std::string ext = fsPath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
		    ext == ".bmp" || ext == ".dds" || ext == ".hdr" || ext == ".webp" || ext == ".wtex" ||
		    ext == ".watlas" || ext == ".wanim" || ext == ".wtilemap" || ext == ".wruletile")
		{
			return AssetCategory::Texture;
		}

		if (ext == ".wmat" || ext == ".mat" || ext == ".material" || ext == ".wphysmat" || ext == ".physmat")
		{
			return AssetCategory::Material;
		}

		if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb" ||
		    ext == ".mesh" || ext == ".wmodel")
		{
			return AssetCategory::Mesh;
		}

		if (ext == ".prefab" || ext == ".wprefab")
		{
			return AssetCategory::Prefab;
		}

		if (ext == ".luau" || ext == ".lua")
		{
			return AssetCategory::Script;
		}

		if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac" ||
		    ext == ".waudio" || ext == ".wsound" || ext == ".wmixer" || ext == ".wvoice")
		{
			return AssetCategory::Audio;
		}

		if (ext == ".wscene" || ext == ".scene" || ext == ".wlevel" || ext == ".level")
		{
			return AssetCategory::Level;
		}

		if (ext == ".wcurve")
		{
			return AssetCategory::Curve;
		}

		if (ext == ".slang")
		{
			return AssetCategory::Shader;
		}

		if (ext == ".wui")
		{
			return AssetCategory::UI;
		}

		if (ext == ".wvfx" || ext == ".wparticle")
		{
			return AssetCategory::VFX;
		}

		return AssetCategory::Unknown;
	}

	void AssetThumbnailCache::RegisterThumbnail(const std::string& path, const ThumbnailInfo& info)
	{
		ThumbnailInfo stored = info;
		stored.Path = path;
		if (stored.Category == AssetCategory::Unknown)
			stored.Category = DetectCategory(path);

		m_Thumbnails[path] = std::move(stored);
	}

	bool AssetThumbnailCache::HasThumbnail(const std::string& path) const
	{
		return m_Thumbnails.find(path) != m_Thumbnails.end();
	}

	const ThumbnailInfo* AssetThumbnailCache::GetThumbnail(const std::string& path) const
	{
		auto it = m_Thumbnails.find(path);
		if (it != m_Thumbnails.end())
			return &it->second;
		return nullptr;
	}

	ThumbnailInfo* AssetThumbnailCache::GetThumbnail(const std::string& path)
	{
		auto it = m_Thumbnails.find(path);
		if (it != m_Thumbnails.end())
			return &it->second;
		return nullptr;
	}

	uint64_t AssetThumbnailCache::GetOrCreateTextureId(const std::string& path)
	{
		auto it = m_Thumbnails.find(path);
		if (it != m_Thumbnails.end() && it->second.TextureId != 0)
			return it->second.TextureId;

		AssetCategory cat = (it != m_Thumbnails.end() && it->second.Category != AssetCategory::Unknown)
			? it->second.Category : DetectCategory(path);

		if (cat == AssetCategory::Texture)
		{
			uint64_t id = Wui::ResolveTextureWithHook(path);
			if (id != 0)
			{
				ThumbnailInfo& info = (it != m_Thumbnails.end()) ? it->second : GetOrCreateThumbnail(path);
				info.TextureId = id;
				info.IsReady = true;
				return id;
			}
		}
		return 0;
	}

	ThumbnailInfo& AssetThumbnailCache::GetOrCreateThumbnail(const std::string& path)
	{
		auto it = m_Thumbnails.find(path);
		if (it != m_Thumbnails.end())
			return it->second;

		ThumbnailInfo info;
		info.Path = path;
		info.Category = DetectCategory(path);
		info.TextureId = 0;
		info.Width = 64;
		info.Height = 64;
		info.IsReady = false;

		auto [insertedIt, success] = m_Thumbnails.emplace(path, std::move(info));
		return insertedIt->second;
	}

	void AssetThumbnailCache::Clear()
	{
		m_Thumbnails.clear();
	}

	size_t AssetThumbnailCache::Count() const
	{
		return m_Thumbnails.size();
	}
}
