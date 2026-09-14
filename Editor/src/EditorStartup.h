#pragma once

#include <string>

namespace World::Editor
{
	// 编辑器启动参数(进程内传递):目前只有"启动即打开的场景"。
	// 不用环境变量是因为 CRT 的 getenv 看不到运行期 SetEnvironmentVariable 的修改。
	void SetStartupScenePath(const std::string& path);
	const std::string& StartupScenePath();
}
