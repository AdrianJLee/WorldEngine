#include "World.h"
#include "EditorCooker.h"
#include "EditorStartup.h"
#include "EditorLayer.h"
#include "EditorPreferences.h"
#include "World/WUI/WuiLocalization.h"
#include "World/Core/Asset/GltfImporter.h"

// 这个文件是整个 Editor 程序的入口，定义了 EditorApp 类并实现了 CreateApplication 函数
#include "World/Core/EntryPoint.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
		// 无窗口打包:`Editor.exe --cook <publishDir> [--scene <relative>] [--check]`。
		// `--check` 只跑资产预检(含脚本编译门),不写发行目录。
		// 与编辑器 Cook 按钮共用 EditorCooker,供 CI/命令行使用(不创建窗口)。
		std::vector<std::string> arguments;
		for (int i = 1; i < __argc; ++i)
			arguments.emplace_back(__argv[i] ? __argv[i] : "");
		// 注意:不能写成 `i + 1 < size` 的配对循环 —— `--ai-control=` 这类**自带数值**的
		// 单个参数会让循环体一次都不执行(实测:通道端口永远是 0,脚本连不上)。
		for (size_t i = 0; i < arguments.size(); ++i)
		{
			// 渲染后端切换后的自动重启会把当前场景带回来。
			if (arguments[i] == "-scene" && i + 1 < arguments.size())
			{
				World::Editor::SetStartupScenePath(arguments[++i]);
				continue;
			}
			// AI 控制通道:`--ai-control=<port>`(默认关闭;只监听 127.0.0.1)。
			if (arguments[i].rfind("--ai-control=", 0) == 0)
			{
				World::Editor::SetAiControlPort(std::atoi(arguments[i].c_str() + std::strlen("--ai-control=")));
				continue;
			}
			// P1b D5:无窗口 glTF 导入 —— `--import-gltf <file> [--out <dir>] [--logical <dir>]`。
			// D10:--logical 指定产物**逻辑目录**(相对内容根,如 models/props);
			// 省略 = 源所在目录(源在内容根外时退回 models/)。
			// 产出 `.wmodel` + `.wmat` + 贴图;失败写 stderr 并以非零码退出(与 --cook 同一风格)。
			if (arguments[i] == "--import-gltf" && i + 1 < arguments.size())
			{
				const std::filesystem::path source = arguments[i + 1];
				std::filesystem::path outputRoot = std::filesystem::path(WLD_ASSETPATH);
				std::string destinationLogicalDir;
				if (i + 2 < arguments.size() && arguments[i + 2] == "--out" && i + 3 < arguments.size())
					outputRoot = arguments[i + 3];
				for (size_t extra = i + 2; extra + 1 < arguments.size(); ++extra)
					if (arguments[extra] == "--logical")
						destinationLogicalDir = arguments[extra + 1];

				World::Asset::GltfImportResult imported;
				std::string importError;
				if (!World::Asset::ImportFile(source, outputRoot, &imported, &importError,
					destinationLogicalDir))
				{
					std::fprintf(stderr, "[import-gltf] FAILED: %s\n", importError.c_str());
					std::exit(1);
				}
				std::printf("[import-gltf] OK: %s (meshes=%u submeshes=%u nodes=%u, materials=%zu, textures=%zu)\n",
					imported.WModelPath.c_str(), imported.MeshCount, imported.SubmeshCount, imported.NodeCount,
					imported.MaterialPaths.size(), imported.TexturePaths.size());
				for (const std::string& material : imported.MaterialPaths)
					std::printf("[import-gltf]   material: %s\n", material.c_str());
				for (const std::string& texture : imported.TexturePaths)
					std::printf("[import-gltf]   texture:  %s\n", texture.c_str());
				std::exit(0);
			}
			if (arguments[i] != "--cook" || i + 1 >= arguments.size())
				continue;
			World::Editor::CookOptions options;
			options.PublishDir = arguments[i + 1];
			size_t next = i + 2;
			if (next + 1 < arguments.size() && arguments[next] == "--scene")
			{
				options.StartSceneOverride = arguments[next + 1];
				next += 2;
			}
			if (next < arguments.size() && arguments[next] == "--check")
				options.CheckOnly = true;

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

		// P4-UX1:编辑器偏好(用户级)必须在 EditorShell 构造之前装载 —— 面板标题/主题/字号
		// 都在创建期取一次快照。语言目录也在编辑器自己的资源里(游戏内容的语言包在 Game 下)。
		World::Wui::SetLocalizationDirectory(std::filesystem::path(WLD_EDITOR_DIR) / "assets" / "localization");
		World::Editor::EditorPreferences::Get().Load(
			std::filesystem::path(WLD_EDITOR_DIR) / "editor-prefs.json");
		return new EditorApp(context);
	}
}
