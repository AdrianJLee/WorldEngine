#pragma once

#include <filesystem>
#include <string>

namespace World::Editor
{
	// 打包(烘焙 + 发行布局)的单一实现:编辑器 UI 的 Cook 按钮与
	// `Editor.exe --cook <dir>` 无窗口模式走同一段代码。
	struct CookOptions
	{
		std::filesystem::path PublishDir;          // 发行目录(会被创建;CheckOnly 时不创建)
		std::filesystem::path StartSceneOverride;  // 可选:覆盖清单里的启动场景(相对内容根)
		bool CheckOnly = false;                    // W7-2:只跑资产导入(含脚本编译门),跳过发行步骤
	};

	struct CookResult
	{
		bool Ok = false;
		std::string Error;
		size_t AssetsTotal = 0;
		size_t AssetsChanged = 0;
		size_t AssetsSkipped = 0;
		size_t ShaderArtifacts = 0;
	};

	// 执行一次完整打包:项目清单 → 增量烘焙 → 着色器烘焙 → 内容包 →
	// Runtime.exe/WorldRuntime.dll/Game.dll → 发行清单。
	CookResult CookProject(const CookOptions& options);
}
