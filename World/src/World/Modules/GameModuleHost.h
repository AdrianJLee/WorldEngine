#pragma once

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
	};
}
