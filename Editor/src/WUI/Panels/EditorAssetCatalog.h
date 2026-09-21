#pragma once

// P4-U9:内容根(Game/assets)下的资产目录扫描 —— 属性面板的"资产路径"下拉、材质面板的
// 贴图槽等共用一份结果。每帧都要列表,所以带 TTL 缓存(默认 2s),不每帧扫盘。

#include "EditorAssetTypes.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace World
{
	namespace Editor::AssetCatalog
	{
		// schema 里的资产类型名(Of("Texture2D") / Asset("Material")…)→ 编辑器资产类型。
		inline EditorAssetKind KindForName(const std::string& assetType)
		{
			if (assetType == "Material") return EditorAssetKind::Material;
			// P4-U10:Model = **引擎原生** .wmodel(场景/字段只引用它);glTF/GLB 是导入源,
			// 单独一个 kind —— 网格选取里绝不该出现 .gltf(用户 2026-09-21 报的歧义)。
			if (assetType == "Model") return EditorAssetKind::Model;
			if (assetType == "ModelSource" || assetType == "glTF" || assetType == "GLTF")
				return EditorAssetKind::ModelSource;
			if (assetType == "Texture" || assetType == "Texture2D") return EditorAssetKind::Texture;
			if (assetType == "Script") return EditorAssetKind::Script;
			if (assetType == "Scene") return EditorAssetKind::Scene;
			return EditorAssetKind::Unknown;
		}

		inline bool MatchesKind(EditorAssetKind kind, const std::filesystem::path& path)
		{
			switch (kind)
			{
				case EditorAssetKind::Material: return LowerExtension(path) == ".wmat";
				// 原生模型资产 = .wmodel(导入产物)。.gltf/.glb 属于 ModelSource,不在"可引用资产"里。
				case EditorAssetKind::Model: return LowerExtension(path) == ".wmodel";
				case EditorAssetKind::ModelSource:
				{
					const std::string extension = LowerExtension(path);
					return extension == ".gltf" || extension == ".glb";
				}
				case EditorAssetKind::Texture:
				{
					const std::string extension = LowerExtension(path);
					return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga";
				}
				case EditorAssetKind::Script:
				{
					const std::string extension = LowerExtension(path);
					return extension == ".lua" || extension == ".luau";
				}
				case EditorAssetKind::Scene: return LowerExtension(path) == ".wd";
				default: return false;
			}
		}

		inline std::vector<std::string> Scan(EditorAssetKind kind)
		{
			std::vector<std::string> paths;
			if (kind == EditorAssetKind::Unknown || kind == EditorAssetKind::Folder)
				return paths;
			std::error_code ec;
			const std::filesystem::path root = std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets";
			if (!std::filesystem::exists(root, ec))
				return paths;
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root,
				std::filesystem::directory_options::skip_permission_denied, ec))
			{
				if (!entry.is_regular_file(ec) || !MatchesKind(kind, entry.path()))
					continue;
				const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, ec);
				if (!ec)
					paths.push_back(relative.generic_string());
			}
			std::sort(paths.begin(), paths.end());
			return paths;
		}

		// 带 TTL 的列表(相对 Game/assets 的逻辑路径,按字典序)。
		inline const std::vector<std::string>& PathsFor(EditorAssetKind kind, double ttlSeconds = 2.0)
		{
			using Clock = std::chrono::steady_clock;
			static std::map<EditorAssetKind, std::pair<Clock::time_point, std::vector<std::string>>> cache;
			static const Clock::time_point start = Clock::now();
			const double now = std::chrono::duration<double>(Clock::now() - start).count();
			auto& entry = cache[kind];
			const double stamp = std::chrono::duration<double>(entry.first - start).count();
			if (stamp <= 0.0 || now - stamp > ttlSeconds)
			{
				entry.first = Clock::now();
				entry.second = Scan(kind);
			}
			return entry.second;
		}

		inline const std::vector<std::string>& PathsForName(const std::string& assetType, double ttlSeconds = 2.0)
		{
			return PathsFor(KindForName(assetType), ttlSeconds);
		}

		// P4-U13d:"创建预制体"对话框的目录下拉 —— 内容根下的目录清单(逻辑路径,字典序)。
		// 与 Scan 同一份缓存口径(默认 2s TTL),低频操作不每帧扫盘;权限错误跳过。
		inline std::vector<std::string> ScanDirs()
		{
			std::vector<std::string> dirs;
			std::error_code ec;
			const std::filesystem::path root = std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets";
			if (!std::filesystem::exists(root, ec))
				return dirs;
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root,
				std::filesystem::directory_options::skip_permission_denied, ec))
			{
				std::error_code itemError;
				if (!entry.is_directory(itemError))
					continue;
				const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, itemError);
				if (itemError || relative.empty())
					continue;
				dirs.push_back(relative.generic_string());
			}
			std::sort(dirs.begin(), dirs.end());
			return dirs;
		}

		inline const std::vector<std::string>& Dirs(double ttlSeconds = 2.0)
		{
			using Clock = std::chrono::steady_clock;
			static std::pair<Clock::time_point, std::vector<std::string>> cache;
			static const Clock::time_point start = Clock::now();
			const double now = std::chrono::duration<double>(Clock::now() - start).count();
			const double stamp = std::chrono::duration<double>(cache.first - start).count();
			if (stamp <= 0.0 || now - stamp > ttlSeconds)
			{
				cache.first = Clock::now();
				cache.second = ScanDirs();
			}
			return cache.second;
		}
	}
}
