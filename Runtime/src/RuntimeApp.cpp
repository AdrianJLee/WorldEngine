#include "wldpch.h"
#include "World.h"
#include "RuntimeLayer.h"
// 这个文件是整个 Runtime 程序的入口,定义了 RuntimeApp 类并实现了 CreateApplication 函数
#include "World/Core/EntryPoint.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiLocalization.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace World
{
	namespace
	{
		// WLD-L10N-S2:发行包的语言层在 **exe 目录**旁 —— `<exe>/localization/{engine,project}/<语言>/**`。
		// 取模块自身路径(GetModuleFileNameA)与进程 CWD 无关:双击 exe、快捷方式启动都成立;
		// 锚定口径与 Game.dll 的打包查找(GameModuleHost.cpp)一致。
		std::filesystem::path ExecutableDirectory()
		{
			char executablePathBuffer[MAX_PATH];
			if (!GetModuleFileNameA(NULL, executablePathBuffer, MAX_PATH))
				return {};
			return std::filesystem::path(executablePathBuffer).parent_path();
		}

		// PROJ-1 P4:启动参数 `--project <dir>`(也可写 `--project=<dir>`)。
		// 必须在 Application 构造**之前**生效:基类构造函数里就会 MountProjectContent()
		// 定位项目清单,放进 RuntimeApp 构造函数体已经太晚。优先级高于 WLD_PROJECT_DIR
		// 环境变量(Paths 内部保证),语义与编辑器侧 --project 一致。
		void ApplyCommandLineProject()
		{
			for (int index = 1; index < __argc; ++index)
			{
				const char* argument = __argv[index];
				if (!argument)
					continue;
				if (std::strcmp(argument, "--project") == 0)
				{
					if (index + 1 >= __argc || !__argv[index + 1] || !__argv[index + 1][0])
					{
						WLD_CORE_WARN("[runtime] --project 缺少目录参数,忽略");
						return;
					}
					World::Paths::SetProjectDirOverride(__argv[index + 1]);
					WLD_CORE_INFO("[runtime] --project override: {0}", World::Paths::ProjectDir().string());
					return;
				}
				if (std::strncmp(argument, "--project=", 10) == 0)
				{
					World::Paths::SetProjectDirOverride(argument + 10);
					WLD_CORE_INFO("[runtime] --project override: {0}", World::Paths::ProjectDir().string());
					return;
				}
			}
		}

		// 包内两层:engine(库自带件/引擎键,priority 0)→ project(项目覆盖 + 项目文案,100)。
		// 目录不存在就不注册(缺哪层算哪层缺;缺翻译永远回退内联英文)。
		// 两层都不存在 = 开发树布局(exe 在 build/x64-<cfg>/Runtime/<cfg>/ 下)→ 保持旧行为:
		// 单个项目语言包目录(`<当前项目根>/assets/localization`)。
		void RegisterRuntimeLocalizationLayers()
		{
			const std::filesystem::path executableDirectory = ExecutableDirectory();
			std::error_code ec;
			const std::filesystem::path engineLayer = executableDirectory / "localization" / "engine";
			const std::filesystem::path projectLayer = executableDirectory / "localization" / "project";
			const bool enginePresent = !executableDirectory.empty()
				&& std::filesystem::is_directory(engineLayer, ec);
			const bool projectPresent = !executableDirectory.empty()
				&& std::filesystem::is_directory(projectLayer, ec);
			if (!enginePresent && !projectPresent)
			{
				Wui::SetLocalizationDirectory(World::Paths::AssetRoot() / "localization");
				WLD_CORE_INFO("[runtime] 包内没有语言层(开发树布局):语言包目录 = {0}",
					Wui::GetLocalizationDirectory().string());
				return;
			}
			Wui::ClearLocalizationLayers();
			if (enginePresent)
				Wui::RegisterLocalizationLayer("engine", engineLayer, 0);
			if (projectPresent)
				Wui::RegisterLocalizationLayer("project", projectLayer, 100);
			WLD_CORE_INFO("[runtime] 包内语言层:engine={0} project={1}",
				enginePresent ? engineLayer.string() : std::string("(missing)"),
				projectPresent ? projectLayer.string() : std::string("(missing)"));
		}
	}

	class RuntimeApp : public Application
	{
	public:
		RuntimeApp(World::WorldContext& context)
			:Application("Runtime", context)
		{
			// 内容挂载(VFS + 着色器产物解析)已由 Application 统一完成:
			// 必须在 Renderer::Init 之前,发行形态才能从内容包读着色器。
			// 语言层必须在 RuntimeLayer 构造(OnAttach 会取文案)之前注册好。
			RegisterRuntimeLocalizationLayers();
			PushLayer(WLD_ENGINE_NEW(RuntimeLayer));
		}

		~RuntimeApp()
		{
		}
	};

	Application* CreateApplication(World::WorldContext& context)
	{
		// 项目根覆盖(如果有)必须先于 Application 构造 —— 那一步就会挂载项目内容。
		ApplyCommandLineProject();
		return new RuntimeApp(context);
	}
}
