#include "wldpch.h"

#include "World/Renderer/MaterialLibrary.h"

#include "World/Core/Log.h"
#include "World/Renderer/MaterialTextureCache.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <unordered_set>

namespace World
{
	namespace
	{
		// 磁盘位置:先内容根(Game/assets,与 MaterialIO::ReadFileText 一致),再 Game/ 布局。
		// 修复前这里只拼 WLD_GAME_DIR / path,普通材质("xxx.wmat" 相对内容根)恒取不到时间戳,
		// IsFileNewer() 因此永远返回 false。
		std::filesystem::path ResolveMaterialDiskPath(const std::string& path)
		{
			std::error_code ec;
			std::filesystem::path candidate = std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets" / path;
			if (std::filesystem::exists(candidate, ec))
				return candidate;
			candidate = std::filesystem::path(std::string(WLD_GAME_DIR)) / path;
			if (std::filesystem::exists(candidate, ec))
				return candidate;
			return {};
		}

		std::filesystem::file_time_type FileWriteTime(const std::string& path)
		{
			std::error_code ec;
			const std::filesystem::path target = ResolveMaterialDiskPath(path);
			if (target.empty())
				return std::filesystem::file_time_type::min();
			const auto time = std::filesystem::last_write_time(target, ec);
			return ec ? std::filesystem::file_time_type::min() : time;
		}

		bool AssetHotReloadEnabled()
		{
			const char* value = std::getenv("WLD_ASSET_HOTRELOAD");
			return !(value && *value && std::string(value) == "0");
		}

		bool AssetHotReloadTraceEnabled()
		{
			const char* value = std::getenv("WLD_ASSET_HOTRELOAD_TRACE");
			return value && *value && std::string(value) != "0";
		}

		void PrintHotReloadTrace(const char* format, const std::string& path)
		{
			if (AssetHotReloadTraceEnabled())
				WLD_CORE_INFO("[asset-hot-reload] {0} {1}", format, path);
		}
	}

	MaterialLibrary& MaterialLibrary::Get()
	{
		// 刻意用堆分配且不析构:与 Renderer 的钩子表同风格,避免静态析构顺序问题。
		static MaterialLibrary* instance = new MaterialLibrary();
		return *instance;
	}

	void MaterialLibrary::Shutdown()
	{
		MaterialLibrary& library = Get();
		library.m_Cache.clear();
		library.m_Warnings.clear();
	}

	std::string MaterialLibrary::NormalizePath(const std::string& path)
	{
		std::string normalized;
		normalized.reserve(path.size());
		for (const char c : path)
			normalized.push_back(c == '\\' ? '/' : c);
		while (normalized.rfind("./", 0) == 0)
			normalized.erase(0, 2);
		// 去掉重复的斜杠(保留协议风格前缀不在本用例范围)。
		normalized.erase(std::unique(normalized.begin(), normalized.end(),
			[](char a, char b) { return a == '/' && b == '/'; }), normalized.end());
		return normalized;
	}

	Ref<Material> MaterialLibrary::Load(const std::string& path, std::string* error)
	{
		const std::string key = NormalizePath(path);
		if (key.empty())
		{
			if (error) *error = "路径为空";
			return nullptr;
		}
		const auto cached = m_Cache.find(key);
		if (cached != m_Cache.end())
		{
			if (error) *error = GetLoadWarning(key);
			return cached->second;
		}

		std::string text;
		if (!MaterialIO::ReadFileText(key, text))
		{
			if (error) *error = "找不到材质文件 " + key;
			return nullptr;
		}

		MaterialDesc desc;
		std::string parseError;
		const MaterialLoadResult result = MaterialIO::Parse(text, desc, &parseError);
		if (!result.Success)
		{
			if (error) *error = result.Error;
			return nullptr;
		}

		Ref<Material> material(new Material(desc, key));
		material->m_FileTime = FileWriteTime(key);
		m_Cache.emplace(key, material);
		if (!result.Error.empty())
			m_Warnings[key] = result.Error;
		if (error) *error = result.Error;
		return material;
	}

	Ref<Material> MaterialLibrary::CreateDefault(const std::string& name)
	{
		MaterialDesc desc;
		desc.Name = name;
		Ref<Material> material(new Material(desc, std::string()));
		material->MarkDirty(true);
		return material;
	}

	bool MaterialLibrary::Save(const Ref<Material>& material, const std::string& path, std::string* error)
	{
		if (!material)
		{
			if (error) *error = "材质为空";
			return false;
		}
		std::string key = NormalizePath(path.empty() ? material->GetPath() : path);
		if (key.empty())
		{
			if (error) *error = "保存路径为空(新建材质需要另存为)";
			return false;
		}
		if (key.size() < 5 || key.substr(key.size() - 5) != ".wmat")
			key.append(".wmat");

		const std::string text = MaterialIO::Serialize(material->GetDesc());
		if (!MaterialIO::WriteFileText(key, text, error))
			return false;

		// 回读校验:确保写出的文件能被自己解析(往返一致的第一道关)。
		MaterialDesc verify;
		std::string verifyError;
		if (!MaterialIO::Parse(text, verify, &verifyError).Success || verify != material->GetDesc())
		{
			if (error) *error = "写入校验失败: " + verifyError;
			return false;
		}

		// 换路径(另存为):旧缓存键让位,实例本身保持同一性(编辑器引用不失效)。
		const std::string previous = material->GetPath();
		if (!previous.empty() && previous != key)
		{
			const auto oldEntry = m_Cache.find(previous);
			if (oldEntry != m_Cache.end() && oldEntry->second == material)
				m_Cache.erase(oldEntry);
			m_Warnings.erase(previous);
		}
		material->SetPath(key);
		material->MarkDirty(false);
		material->InvalidateTextures();   // 磁盘内容变化 → 贴图/GPU 侧重建
		material->m_FileTime = FileWriteTime(key);
		m_Cache[key] = material;
		m_Warnings.erase(key);
		return true;
	}

	bool MaterialLibrary::Reload(const std::string& path, std::string* error)
	{
		const std::string key = NormalizePath(path);
		std::string text;
		if (!MaterialIO::ReadFileText(key, text))
		{
			if (error) *error = "找不到材质文件 " + key;
			return false;
		}
		MaterialDesc desc;
		std::string parseError;
		const MaterialLoadResult result = MaterialIO::Parse(text, desc, &parseError);
		if (!result.Success)
		{
			if (error) *error = result.Error;
			return false;
		}

		const auto cached = m_Cache.find(key);
		if (cached != m_Cache.end())
		{
			// 原地更新:保留实例(编辑器/渲染侧的 Ref 不失效),Revision 触发 GPU 侧重建。
			cached->second->SetDesc(desc);
			cached->second->MarkDirty(false);
			cached->second->m_FileTime = FileWriteTime(key);
		}
		else
		{
			Ref<Material> material(new Material(desc, key));
			material->m_FileTime = FileWriteTime(key);
			m_Cache.emplace(key, material);
		}

		if (result.Error.empty())
			m_Warnings.erase(key);
		else
			m_Warnings[key] = result.Error;
		if (error) *error = result.Error;
		return true;
	}

	bool MaterialLibrary::IsFileNewer(const Material& material) const
	{
		if (material.GetPath().empty())
			return false;
		const auto time = FileWriteTime(material.GetPath());
		if (time == std::filesystem::file_time_type::min())
			return false;
		return time > material.m_FileTime;
	}

	void MaterialLibrary::PollAssetChanges(double deltaSeconds, AssetHotReloadReport& report)
	{
		if (!AssetHotReloadEnabled())
		{
			// 整体关闭:不建立也不推进监听(重新开启时重新建立基线)。
			m_MaterialWatch.Clear();
			m_TextureWatch.Clear();
			return;
		}

		// ---- 监听集合同步:材质 = 当前缓存;贴图 = 缓存材质引用的 Albedo/Normal ----
		std::vector<std::string> wantedMaterials;
		wantedMaterials.reserve(m_Cache.size());
		std::vector<std::string> wantedTextures;
		const auto addTexture = [&wantedTextures](const std::string& path)
		{
			if (path.empty())
				return;
			const std::string normalized = MaterialLibrary::NormalizePath(path);
			if (normalized.empty())
				return;
			if (std::find(wantedTextures.begin(), wantedTextures.end(), normalized) == wantedTextures.end())
				wantedTextures.push_back(normalized);
		};
		for (const auto& [key, material] : m_Cache)
		{
			wantedMaterials.push_back(key);
			if (!material)
				continue;
			addTexture(material->GetDesc().AlbedoTexture);
			addTexture(material->GetDesc().NormalTexture);
		}
		std::sort(wantedMaterials.begin(), wantedMaterials.end());
		std::sort(wantedTextures.begin(), wantedTextures.end());

		const std::vector<std::string> watchedMaterials = m_MaterialWatch.WatchedPaths();
		for (const std::string& path : wantedMaterials)
			if (std::find(watchedMaterials.begin(), watchedMaterials.end(), path) == watchedMaterials.end())
				m_MaterialWatch.Watch(path);
		for (const std::string& path : watchedMaterials)
			if (std::find(wantedMaterials.begin(), wantedMaterials.end(), path) == wantedMaterials.end())
				m_MaterialWatch.Unwatch(path);

		const std::vector<std::string> watchedTextures = m_TextureWatch.WatchedPaths();
		for (const std::string& path : wantedTextures)
			if (std::find(watchedTextures.begin(), watchedTextures.end(), path) == watchedTextures.end())
				m_TextureWatch.Watch(path);
		for (const std::string& path : watchedTextures)
			if (std::find(wantedTextures.begin(), wantedTextures.end(), path) == wantedTextures.end())
				m_TextureWatch.Unwatch(path);
		if (AssetHotReloadTraceEnabled())
			WLD_CORE_INFO("[asset-hot-reload] watching {0} material(s), {1} texture(s)",
				wantedMaterials.size(), wantedTextures.size());

		// ---- 材质:.wmat 内容变化(已过 debounce)→ clean 原地重载 / dirty 只报告 ----
		for (const std::string& path : m_MaterialWatch.Poll(deltaSeconds))
		{
			const auto entry = m_Cache.find(path);
			if (entry == m_Cache.end() || !entry->second)
				continue;   // 已不在缓存(轮询与缓存同步之间消失)
			if (entry->second->IsDirty())
			{
				// 有未保存修改:只报告,绝不覆盖(面板的脏标记由编辑器维护)。
				report.SkippedDirtyMaterials.push_back(path);
				PrintHotReloadTrace("skip dirty material", path);
				continue;
			}
			std::string error;
			if (Reload(path, &error))
			{
				report.ReloadedMaterials.push_back(path);
				PrintHotReloadTrace("reloaded material", path);
			}
			else
			{
				// 读取/解析失败:保留旧内存态(Reload 失败不改实例)。
				report.FailedMaterials.push_back(AssetReloadFailure { path, error });
				PrintHotReloadTrace("failed material", path);
			}
		}

		// ---- 贴图:内容变化 → 清 s:/l: 缓存 + 引用方 Revision 前进(旧句柄延迟释放) ----
		const std::vector<std::string> changedTextures = m_TextureWatch.Poll(deltaSeconds);
		if (!changedTextures.empty())
		{
			std::unordered_set<std::string> changed(changedTextures.begin(), changedTextures.end());
			for (const std::string& path : changedTextures)
			{
				const std::string normalized = MaterialLibrary::NormalizePath(path);
				// 清 s:/l: 两份;旧句柄由缓存内部按 Renderer::QueueRelease 延迟释放
				// (GL 立即、Vulkan 三帧/fence 后;无设备时安全 no-op)。
				MaterialTextureCache::Get().Invalidate(normalized);
			}

			for (const auto& [key, material] : m_Cache)
			{
				if (!material)
					continue;
				const MaterialDesc& desc = material->GetDesc();
				const bool albedoChanged = !desc.AlbedoTexture.empty()
					&& changed.count(MaterialLibrary::NormalizePath(desc.AlbedoTexture)) != 0;
				const bool normalChanged = !desc.NormalTexture.empty()
					&& changed.count(MaterialLibrary::NormalizePath(desc.NormalTexture)) != 0;
				if (albedoChanged || normalChanged)
					material->InvalidateTextures();
			}
			for (const std::string& normalized : changedTextures)
			{
				report.InvalidatedTextures.push_back(normalized);
				PrintHotReloadTrace("invalidated texture", normalized);
			}
		}
	}

	std::string MaterialLibrary::GetLoadWarning(const std::string& path) const
	{
		const auto it = m_Warnings.find(NormalizePath(path));
		return it == m_Warnings.end() ? std::string() : it->second;
	}

	std::vector<std::string> MaterialLibrary::ScanMaterials() const
	{
		std::vector<std::string> paths;
		std::error_code ec;
		// 内容根 = Game/assets(与材质路径的书写约定一致);遍历失败时返回空列表。
		const std::filesystem::path root = std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets";
		if (!std::filesystem::exists(root, ec))
			return paths;
		for (const std::filesystem::directory_entry& entry :
			std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec))
		{
			if (!entry.is_regular_file(ec) || entry.path().extension() != ".wmat")
				continue;
			const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, ec);
			if (!ec)
				paths.push_back(NormalizePath(relative.generic_string()));
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	}
}
