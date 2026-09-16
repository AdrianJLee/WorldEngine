#include "wldpch.h"

#include "World/Renderer/MaterialLibrary.h"

#include <algorithm>
#include <filesystem>
#include <memory>

namespace World
{
	namespace
	{
		std::filesystem::file_time_type FileWriteTime(const std::string& path)
		{
			std::error_code ec;
			const std::filesystem::path target = std::filesystem::path(std::string(WLD_GAME_DIR)) / path;
			const auto time = std::filesystem::last_write_time(target, ec);
			return ec ? std::filesystem::file_time_type::min() : time;
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
