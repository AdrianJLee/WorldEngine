#pragma once

#include "World/Core/Export.h"
#include <cstdint>
#include <string_view>

namespace World
{
	enum class EngineHostMode : uint8_t
	{
		Editor = 0,     // 编辑器宿主 (可切换 Edit / Play / Simulate)
		Runtime = 1,    // 独立开发期运行时宿主 (Runtime.exe 开发环境)
		Packaged = 2,   // 烘焙打包后的正式发布独立游戏 (Shipping Standalone)
		Headless = 3,   // 自动化测试 / CI 无头宿主
		Tool = 4        // 离线工具 (Cooker, AssetCompiler, CLI)
	};

	class WLD_API EngineEnvironment
	{
	public:
		static void SetHostMode(EngineHostMode mode);
		static EngineHostMode GetHostMode();

		static bool IsEditor();
		static bool IsRuntime();
		static bool IsPackaged();
		static bool IsHeadless();
		static bool IsTool();

		// 表现层判断: 是否处于非编辑器的纯游戏运行环境 (Runtime 或 Packaged)
		static bool IsPureGameRuntime();

		static std::string_view GetHostModeName();
		static std::string_view HostModeToString(EngineHostMode mode);
	};
}
