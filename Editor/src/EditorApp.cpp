#include "World.h"
#include "EditorCooker.h"
#include "EditorLayer.h"

// 这个文件是整个 Editor 程序的入口，定义了 EditorApp 类并实现了 CreateApplication 函数
#include "World/Core/EntryPoint.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace World
{
	class EditorApp : public Application
	{
	public:
		EditorApp(World::WorldContext& context)
			:Application("Editor", context)
		{
			PushLayer(WLD_ENGINE_NEW(EditorLayer));
		}
		~EditorApp()
		{

		}
	};

	Application* CreateApplication(World::WorldContext& context)
	{
		// 无窗口打包:`Editor.exe --cook <publishDir> [--scene <relative>]`。
		// 与编辑器 Cook 按钮共用 EditorCooker,供 CI/命令行使用(不创建窗口)。
		std::vector<std::string> arguments;
		for (int i = 1; i < __argc; ++i)
			arguments.emplace_back(__argv[i] ? __argv[i] : "");
		for (size_t i = 0; i + 1 < arguments.size(); ++i)
		{
			if (arguments[i] != "--cook")
				continue;
			World::Editor::CookOptions options;
			options.PublishDir = arguments[i + 1];
			if (i + 3 < arguments.size() && arguments[i + 2] == "--scene")
				options.StartSceneOverride = arguments[i + 3];

			std::printf("[cook] publish dir: %s\n", options.PublishDir.string().c_str());
			const World::Editor::CookResult cooked = World::Editor::CookProject(options);
			if (cooked.Ok)
			{
				std::printf("[cook] OK: %zu assets (%zu changed), %zu shader artifacts\n",
					cooked.AssetsTotal, cooked.AssetsChanged, cooked.ShaderArtifacts);
				std::exit(0);
			}
			std::fprintf(stderr, "[cook] FAILED: %s\n", cooked.Error.c_str());
			std::exit(1);
		}

		return new EditorApp(context);
	}
}
