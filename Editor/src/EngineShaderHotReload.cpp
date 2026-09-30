#include "EngineShaderHotReload.h"

#include "World/Core/Log.h"
#include "World/Renderer/Renderer.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

namespace World::Editor
{
	namespace
	{
		// 引擎 shader 目录的唯一来源:构建期锚点 WLD_WORLD_DIR(= <引擎根>/Engine/)。
		std::filesystem::path DefaultRoot()
		{
#ifdef WLD_WORLD_DIR
			return std::filesystem::path(WLD_WORLD_DIR) / "assets" / "shaders";
#else
			return {};
#endif
		}

		// FNV-1a64:与 AssetHotReload / Script.HotReload 同族(字典序无关的稳定内容哈希)。
		uint64_t HashBytes(const void* data, std::size_t size)
		{
			constexpr uint64_t offsetBasis = 0xcbf29ce484222325ull;
			constexpr uint64_t prime = 0x100000001b3ull;
			uint64_t hash = offsetBasis;
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (std::size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= prime;
			}
			return hash;
		}
	}

	bool EngineShaderHotReload::HashFile(const std::filesystem::path& path, uint64_t& out)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
			return false;
		const std::streamsize size = file.tellg();
		if (size < 0)
			return false;
		std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
		file.seekg(0, std::ios::beg);
		if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size))
			return false;
		out = bytes.empty() ? 0xcbf29ce484222325ull : HashBytes(bytes.data(), bytes.size());
		return true;
	}

	bool EngineShaderHotReload::Available() const
	{
		std::error_code error;
		return !m_Root.empty() && std::filesystem::is_directory(m_Root, error);
	}

	void EngineShaderHotReload::Shutdown()
	{
		m_Stable.clear();
		m_Pending.clear();
		m_PendingElapsed = 0.0;
		m_HasBaseline = false;
		m_HasPending = false;
	}

	void EngineShaderHotReload::Scan(std::unordered_map<std::string, uint64_t>& out) const
	{
		out.clear();
		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(
			m_Root, std::filesystem::directory_options::skip_permission_denied, error);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end)
		{
			const std::filesystem::directory_entry& entry = *iterator;
			std::error_code entryError;
			if (entry.is_regular_file(entryError) && entry.path().extension() == ".slang")
			{
				uint64_t hash = 0;
				if (HashFile(entry.path(), hash))
				{
					const std::filesystem::path relative =
						std::filesystem::relative(entry.path(), m_Root, entryError);
					const std::string key = entryError ? entry.path().filename().string()
						: relative.generic_string();
					out[key] = hash;
				}
			}
			iterator.increment(error);
		}
	}

	void EngineShaderHotReload::ReportChanges(const std::unordered_map<std::string, uint64_t>& before,
		const std::unordered_map<std::string, uint64_t>& after) const
	{
		for (const auto& [name, hash] : after)
		{
			const auto found = before.find(name);
			if (found == before.end())
				WLD_CORE_INFO("[shader-hot-reload] engine shader added: {0}", name);
			else if (found->second != hash)
				WLD_CORE_INFO("[shader-hot-reload] engine shader changed: {0}", name);
		}
		for (const auto& [name, hash] : before)
		{
			if (after.find(name) == after.end())
				WLD_CORE_INFO("[shader-hot-reload] engine shader removed: {0}", name);
		}
	}

	bool EngineShaderHotReload::Poll(double deltaSeconds)
	{
		if (m_Root.empty())
			m_Root = DefaultRoot();
		if (!Available())
			return false;
		// 开发/自动化开关:WLD_SHADER_HOTRELOAD=0 整体关闭引擎 shader 监听。
		if (const char* switchValue = std::getenv("WLD_SHADER_HOTRELOAD"))
		{
			if (std::string(switchValue) == "0")
				return false;
		}

		std::unordered_map<std::string, uint64_t> current;
		Scan(current);

		if (!m_HasBaseline)
		{
			// 首次:只建立基线(不触发重载)。
			m_Stable = std::move(current);
			m_Pending.clear();
			m_PendingElapsed = 0.0;
			m_HasBaseline = true;
			m_HasPending = false;
			WLD_CORE_INFO("[shader-hot-reload] engine shader watch baseline: {0} file(s) under {1}",
				m_Stable.size(), m_Root.string());
			return false;
		}

		if (current == m_Stable)
		{
			// 回到已确认内容(或空转):撤销未决变化。
			m_Pending.clear();
			m_PendingElapsed = 0.0;
			m_HasPending = false;
			return false;
		}

		if (!m_HasPending || current != m_Pending)
		{
			// 新的未决内容:重新计时(消抖窗口内再变则从头计)。
			m_Pending = std::move(current);
			m_HasPending = true;
			m_PendingElapsed = 0.0;
			return false;
		}

		m_PendingElapsed += std::max(0.0, deltaSeconds);
		if (m_PendingElapsed < kDebounceSeconds)
			return false;

		// 稳定变化:报差异 → 新基线 → 同一入口(与 AI `renderer.reload_shaders` 相同)重建。
		ReportChanges(m_Stable, m_Pending);
		m_Stable = m_Pending;
		m_Pending.clear();
		m_PendingElapsed = 0.0;
		m_HasPending = false;

		const ShaderReloadResult result = Renderer::ReloadShaders();
		WLD_CORE_INFO("[shader-hot-reload] engine shader change triggered reload: owners={0} "
			"pipelines={1} failed={2}{3}", result.Owners, result.Pipelines, result.Failed,
			result.Error.empty() ? std::string() : (" (" + result.Error + ")"));
		return true;
	}
}
