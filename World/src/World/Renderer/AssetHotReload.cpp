#include "wldpch.h"

#include "World/Renderer/AssetHotReload.h"

#include "World/Renderer/Material.h"

#include "World/Core/Application.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace World
{
	namespace
	{
		// FNV-1a64:与 Script/HotReload 同族算法(字典序无关的稳定内容哈希)。
		// 刻意不 include Script/HotReload.h:工具层、资产层都依赖它,保持通用。
		constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
		constexpr uint64_t kFnvPrime = 0x100000001b3ull;

		uint64_t HashBytes(const void* data, std::size_t size)
		{
			uint64_t hash = kFnvOffsetBasis;
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (std::size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= kFnvPrime;
			}
			return hash;
		}

		// 逻辑路径的磁盘位置:内容根 = WLD_ASSETPATH(Game/assets),与材质路径书写约定一致。
		std::filesystem::path DiskPathFor(const std::string& logicalPath)
		{
			return std::filesystem::path(WLD_ASSETPATH) / logicalPath;
		}

		// M3:.wmat 是材质实例(覆盖字段 + Parent),指纹必须含**解析后的父级链**,
		// 否则"父级改了、子材质没变"会漏检。深度上限与父级链一致(8 级);
		// 环由 MaterialLibrary::Load 拒绝,这里只防指纹递归本身失控。
		constexpr int kMaxMaterialParentDepth = 8;

		AssetFingerprint FingerprintAssetInternal(const std::string& logicalPath, std::string* error, int depth);

		bool IsMaterialPath(const std::string& logicalPath)
		{
			return logicalPath.size() >= 5 && logicalPath.compare(logicalPath.size() - 5, 5, ".wmat") == 0;
		}

		// FNV-1a 风格混合:把父级指纹并入子级(顺序敏感、稳定、与字节哈希同族)。
		void MixFingerprint(uint64_t& value, uint64_t other)
		{
			value ^= other;
			value *= kFnvPrime;
		}
	}

	bool ReadAssetBytes(const std::string& logicalPath, std::vector<uint8_t>& out, std::string* error)
	{
		if (logicalPath.empty())
		{
			if (error) *error = "asset path is empty";
			return false;
		}

		// 1) VFS 优先:开发目录 provider 每次 Read 都走 provider,改盘立即可见。
		if (Application::HasInstance())
		{
			std::error_code vfsError;
			std::vector<uint8_t> bytes;
			if (Application::Get().GetContext().Vfs().Read(logicalPath, bytes, vfsError) && !bytes.empty())
			{
				out = std::move(bytes);
				if (error) error->clear();
				return true;
			}
		}

		// 2) 磁盘回退(未挂载 VFS 的 headless / 开发环境)。
		std::ifstream file(DiskPathFor(logicalPath), std::ios::binary);
		if (!file.is_open())
		{
			if (error) *error = "asset not found: " + logicalPath;
			return false;
		}
		std::stringstream buffer;
		buffer << file.rdbuf();
		const std::string text = buffer.str();
		out.assign(text.begin(), text.end());
		if (error) error->clear();
		return true;
	}

	namespace
	{
		AssetFingerprint FingerprintAssetInternal(const std::string& logicalPath, std::string* error, int depth)
		{
			AssetFingerprint result;
			if (logicalPath.empty())
			{
				if (error) *error = "asset path is empty";
				return result;
			}

			// 内容哈希优先:同内容重写(只动 mtime)不算变化。
			std::vector<uint8_t> bytes;
			std::string readError;
			if (ReadAssetBytes(logicalPath, bytes, &readError))
			{
				result.Value = (bytes.empty() ? kFnvOffsetBasis : HashBytes(bytes.data(), bytes.size()));
				result.FromContent = true;
				result.Exists = true;

				// M3:材质实例指纹 = 本文件内容 ⊕ 解析后父级链的指纹(父改 → 子失效)。
				if (IsMaterialPath(logicalPath) && depth < kMaxMaterialParentDepth)
				{
					const std::string text(bytes.begin(), bytes.end());
					MaterialDocument document;
					if (MaterialIO::ParseDocument(text, document, nullptr).Success && !document.ParentPath.empty())
					{
						const AssetFingerprint parent =
							FingerprintAssetInternal(document.ParentPath, nullptr, depth + 1);
						MixFingerprint(result.Value, parent.Value);
						MixFingerprint(result.Value, parent.Exists ? 0x9e3779b97f4a7c15ull : 0ull);
					}
				}
				if (error) error->clear();
				return result;
			}

			// 内容读不到 → size|mtime 兜底(只有磁盘能提供;包内读取失败时自然退化成不存在)。
			std::error_code ec;
			const std::filesystem::path diskPath = DiskPathFor(logicalPath);
			const uintmax_t size = std::filesystem::file_size(diskPath, ec);
			if (ec)
			{
				if (error) *error = readError;
				return result;
			}
			const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(diskPath, ec);
			if (ec)
			{
				if (error) *error = readError;
				return result;
			}
			const std::string key = std::to_string(static_cast<unsigned long long>(size)) + "|" +
				std::to_string(static_cast<long long>(writeTime.time_since_epoch().count()));
			result.Value = HashBytes(key.data(), key.size());
			result.FromContent = false;
			result.Exists = true;
			if (error) error->clear();
			return result;
		}
	}

	AssetFingerprint FingerprintAsset(const std::string& logicalPath, std::string* error)
	{
		return FingerprintAssetInternal(logicalPath, error, 0);
	}

	AssetFileWatch::AssetFileWatch(double debounceSeconds)
	{
		if (std::isfinite(debounceSeconds) && debounceSeconds > 0.0)
			m_DebounceSeconds = debounceSeconds;
	}

	void AssetFileWatch::Watch(const std::string& logicalPath)
	{
		if (logicalPath.empty())
			return;
		Entry entry;
		entry.Stable = FingerprintAsset(logicalPath);
		entry.Pending = entry.Stable;
		m_Entries[logicalPath] = std::move(entry);
	}

	void AssetFileWatch::Unwatch(const std::string& logicalPath)
	{
		m_Entries.erase(logicalPath);
	}

	void AssetFileWatch::Clear()
	{
		m_Entries.clear();
	}

	bool AssetFileWatch::IsWatched(const std::string& logicalPath) const
	{
		return m_Entries.find(logicalPath) != m_Entries.end();
	}

	std::vector<std::string> AssetFileWatch::WatchedPaths() const
	{
		std::vector<std::string> paths;
		paths.reserve(m_Entries.size());
		for (const auto& entry : m_Entries)
			paths.push_back(entry.first);
		std::sort(paths.begin(), paths.end());
		return paths;
	}

	std::vector<std::string> AssetFileWatch::Poll(double deltaSeconds)
	{
		std::vector<std::string> changed;
		if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0)
			deltaSeconds = 0.0;

		for (auto& [path, entry] : m_Entries)
		{
			const AssetFingerprint current = FingerprintAsset(path);
			if (current.Exists == entry.Stable.Exists && current.Value == entry.Stable.Value)
			{
				// 内容没变,或改了又改回已确认内容 → 撤销未决变化。
				entry.HasPending = false;
				entry.Elapsed = 0.0;
				continue;
			}
			if (!entry.HasPending || current.Value != entry.Pending.Value
				|| current.Exists != entry.Pending.Exists)
			{
				// 观察到一份新的未决内容:重启 debounce 窗口(连续写归并成一次)。
				entry.Pending = current;
				entry.HasPending = true;
				entry.Elapsed = 0.0;
				continue;
			}
			entry.Elapsed += deltaSeconds;
			if (entry.Elapsed >= m_DebounceSeconds)
			{
				entry.Stable = entry.Pending;
				entry.HasPending = false;
				entry.Elapsed = 0.0;
				changed.push_back(path);
			}
		}

		std::sort(changed.begin(), changed.end());
		return changed;
	}
}
