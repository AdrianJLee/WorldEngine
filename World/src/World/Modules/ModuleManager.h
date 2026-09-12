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
		};

		static const char* StatusName(Status status);

		Status Load(const std::filesystem::path& path, WorldContext& context, std::string* error = nullptr);
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
