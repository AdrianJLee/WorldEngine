#pragma once

#include "World/Modules/WeModule.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	class WorldContext;
	class DynamicLibrary;
}

namespace World::Modules
{
	// 宿主唯一持有的模块管理器。加载失败不留下半注册状态;退出时逆序卸载。
	class ModuleManager
	{
	public:
		enum class Status : uint8_t
		{
			Ok,
			NotFound,
			LoadFailed,
			MissingEntry,
			AbiMismatch,
			RegistrationFailed,
			AlreadyLoaded,   // CPPT-2:Load 时同 id 已加载(热重载必须先 Unload)
			NotSafePoint,    // CPPT-2:模块热重载被安全点检查拒绝
		};

		static const char* StatusName(Status status);

		Status Load(const std::filesystem::path& path, WorldContext& context, std::string* error = nullptr);
		// CPPT-2(T5b 模块级热重载):单模块卸载(注销 schema + 注销该模块的脚本行为描述 +
		// 释放 DLL)与按 id 查找。卸载顺序与 UnloadAll 同向:先注销、后卸载库。
		Status UnloadAt(size_t index, WorldContext& context, std::string* error = nullptr);
		Status Unload(const std::string& moduleId, WorldContext& context, std::string* error = nullptr);
		// 当前已加载模块的 id / 路径;没有该模块时返回 nullptr / 空路径。
		const WeModule* FindById(const std::string& moduleId) const;
		std::filesystem::path PathOf(const std::string& moduleId) const;
		void UnloadAll(WorldContext& context);
		size_t Count() const { return m_Entries.size(); }

	private:
		struct Entry
		{
			std::string Path;
			std::unique_ptr<World::DynamicLibrary> Library;
			const WeModule* Module = nullptr;
		};

		std::vector<Entry> m_Entries;
	};
}
