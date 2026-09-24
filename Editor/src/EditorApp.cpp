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
	namespace
	{
		// WLD-L10N-S2:语言包热重载轮询(0.5s 节流;消费 S3 的 `LocalizationFilesChanged`)。
		//
		//  - 只在**非内联默认语言**下查盘:英文(en/空)按 P4-UX1 口径不读语言包,盘上文件变化
		//    不该触发重扫,更不该为它刷盘;
		//  - `LocalizationFilesChanged()` 只比较"已加载输入"的(路径, 大小, mtime),不自己重载 ——
		//    何时重载由宿主决定;true → `ReloadLocalization()` 立即重扫(Generation++),
		//    面板每帧经 `Tr` 取文案,下一帧就读到新内容,不需要重启。
		class LocalizationHotReloadLayer : public Layer
		{
		public:
			LocalizationHotReloadLayer()
				:Layer("LocalizationHotReload")
			{
			}

			void OnUpdate(Timestep ts) override
			{
				m_Elapsed += ts.GetSeconds();
				if (m_Elapsed < kInterval)
					return;
				m_Elapsed = 0.0f;
				const std::string& language = Wui::GetLanguage();
				if (language.empty() || language == "en" || language == "en-US")
					return;   // 内联默认语言:没有语言包参与,不查盘
				if (!Wui::LocalizationFilesChanged())
					return;
				Wui::ReloadLocalization();
				WLD_CORE_INFO("[i18n] 语言包已变化,热重载完成(语言 {0};generation {1})",
					language, Wui::LocalizationGeneration());
			}

		private:
			static constexpr float kInterval = 0.5f;
			float m_Elapsed = kInterval;   // 首帧即做第一次检查(不等 0.5s)
		};
	}

	class EditorApp : public Application
	{
	public:
		EditorApp(World::WorldContext& context)
			:Application("Editor", context)
		{
			PushLayer(WLD_ENGINE_NEW(EditorLayer));
			// 热重载轮询层:改语言包文件后不重启即生效(与 EditorLayer 同栈,每帧 OnUpdate)。
			PushLayer(WLD_ENGINE_NEW(LocalizationHotReloadLayer));
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
			// 可选参数(顺序无关):`--scene <相对路径>`、`--check`、
			// `--languages=zh-CN,en`(WLD-L10N-S2:发行包只带选中的语言;不写 = 扫描到的全部语言)。
			for (size_t next = i + 2; next < arguments.size(); )
			{
				if (arguments[next] == "--scene" && next + 1 < arguments.size())
				{
					options.StartSceneOverride = arguments[next + 1];
					next += 2;
					continue;
				}
				if (arguments[next] == "--check")
				{
					options.CheckOnly = true;
					++next;
					continue;
				}
				if (arguments[next].rfind("--languages=", 0) == 0)
				{
					const std::string list = arguments[next].substr(std::strlen("--languages="));
					size_t start = 0;
					for (;;)
					{
						const size_t comma = list.find(',', start);
						std::string language = list.substr(start,
							comma == std::string::npos ? std::string::npos : comma - start);
						// 去首尾空白:`--languages=zh-CN, en` 也接受。
						const size_t first = language.find_first_not_of(" \t");
						const size_t last = language.find_last_not_of(" \t");
						language = first == std::string::npos ? std::string()
							: language.substr(first, last - first + 1);
						if (!language.empty())
							options.Languages.push_back(language);
						if (comma == std::string::npos)
							break;
						start = comma + 1;
					}
					++next;
					continue;
				}
				break;   // 未知参数:到此为止(与旧的两段式解析同样的宽容度)
			}

			std::string languageList;
			for (const std::string& language : options.Languages)
			{
				if (!languageList.empty())
					languageList += ",";
				languageList += language;
			}
			std::printf("[cook] publish dir: %s (languages: %s)\n", options.PublishDir.string().c_str(),
				languageList.empty() ? "(all)" : languageList.c_str());
			const World::Editor::CookResult cooked = World::Editor::CookProject(options);
			if (cooked.Ok)
			{
				std::printf("[cook] OK: %zu assets (%zu changed), %zu shader artifacts, "
					"%zu localization files (%zu languages)\n",
					cooked.AssetsTotal, cooked.AssetsChanged, cooked.ShaderArtifacts,
					cooked.LocalizationFiles, cooked.LocalizationLanguages);
				std::exit(0);
			}
			std::fprintf(stderr, "[cook] FAILED: %s\n", cooked.Error.c_str());
			std::exit(1);
		}

		// P4-UX1:编辑器偏好(用户级)必须在 EditorShell 构造之前装载 —— 面板标题/主题/字号
		// 都在创建期取一次快照。语言目录也在编辑器自己的资源里(游戏内容的语言包在
		// `projects/<名>/assets/localization` 下)。
		// WLD-L10N-S1:语言包改成三层注册 —— engine(引擎自带件)→ editor(编辑器 UI)→ project
		// (项目覆盖),priority 升序解析,后者覆盖前者;每层目录下 `<语言>/**/*.json` 自动扫描,
		// 缺一层只告警(回退内联英文),不影响其它层。项目层可覆盖引擎/编辑器文案。
		World::Wui::ClearLocalizationLayers();
		World::Wui::RegisterLocalizationLayer("engine",
			std::filesystem::path(WLD_WORLD_DIR) / "assets" / "localization", 0);
		World::Wui::RegisterLocalizationLayer("editor",
			std::filesystem::path(WLD_EDITOR_DIR) / "assets" / "localization", 100);
		World::Wui::RegisterLocalizationLayer("project",
			std::filesystem::path(WLD_PROJECT_DIR) / "assets" / "localization", 200);
		// 本机状态目录(编辑器偏好/布局/窗口/最近使用):不入库;首次运行自动创建。
		std::filesystem::create_directories(std::filesystem::path(WLD_LOCAL_DIR));
		World::Editor::EditorPreferences::Get().Load(
			std::filesystem::path(WLD_LOCAL_DIR) / "editor-prefs.json");
		// P4-UX7:AI 控制通道端口 —— 命令行 `--ai-control=<port>` 优先(自动化脚本),
		// 没给命令行时用编辑器偏好里的端口(面板可见、重启生效)。
		if (World::Editor::AiControlPort() <= 0)
		{
			const int preferencePort = World::Editor::EditorPreferences::Get().Data().AiControlPort;
			if (preferencePort > 0)
			{
				World::Editor::SetAiControlPort(preferencePort);
				WLD_CORE_INFO("AI 控制通道端口来自编辑器偏好: {0}", preferencePort);
			}
		}
		return new EditorApp(context);
	}
}
