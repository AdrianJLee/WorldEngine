#pragma once

#include <filesystem>
#include <string>

namespace World
{
	class WorldContext;
}

namespace World::Modules
{
	// Editor/Runtime 共用的 Game 模块加载。P0 维持现有开发布局:
	// exe 旁 bin 目录递归查找 Game.dll,否则退回构建目录。
	class GameModuleHost
	{
	public:
		static bool LoadDefault(WorldContext& context, std::string* error = nullptr);
		// CPPT-2(T5b):按与 LoadDefault 相同的候选顺序解析 Game.dll 的**路径**(不加载)。
		// 热重载在卸载后重编该文件、再按同一条路径加载;找不到时返回开发布局锚定路径
		// (交给 ModuleManager 报可读的 NotFound,不在这里静默回退到别的目录)。
		static std::filesystem::path ResolveDefaultPath();
	};
}
