#include "wldpch.h"
#include "World.h"
#include "RuntimeLayer.h"
// 这个文件是整个 Runtime 程序的入口,定义了 RuntimeApp 类并实现了 CreateApplication 函数
#include "World/Core/EntryPoint.h"
#include "World/WUI/WuiLocalization.h"

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

		// 包内两层:engine(库自带件/引擎键,priority 0)→ project(项目覆盖 + 项目文案,100)。
		// 目录不存在就不注册(缺哪层算哪层缺;缺翻译永远回退内联英文)。
		// 两层都不存在 = 开发树布局(exe 在 build/x64-<cfg>/Runtime/<cfg>/ 下)→ 保持旧行为:
		// 单个项目语言包目录(`WLD_PROJECT_DIR/assets/localization`)。
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
				Wui::SetLocalizationDirectory(std::filesystem::path(WLD_PROJECT_DIR) / "assets" / "localization");
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
		return new RuntimeApp(context);
	}
}
