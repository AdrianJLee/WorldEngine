#include "wldpch.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Modules/ModuleManager.h"
#include "World/Core/WorldContext.h"

#include <filesystem>
#include <vector>

namespace World::Modules
{
	namespace
	{
		// 开发期布局的唯一口径:<仓库根>/build/x64-<cfg>/bin/<cfg>/Game/<cfg>/Game.dll。
		//
		// WLD_OUTPUT_DIR 由 CMake 定义成**相对**路径("build/x64-<cfg>/"),按进程 CWD 解析:
		// CWD 不是仓库根时(双击 exe、从构建目录或快捷方式启动)旧兜底会静默找不到 Game.dll,
		// Game 模块不注册 → Game 组件不进 schema(编辑器的 Lua 存根会被改写成缺 Game 组件块,
		// Layout-S6 已因此修过一次)。这里改用**绝对**编译期锚点 WLD_REPO_ROOT(Layout-S7 新增,
		// 不带尾分隔符)把它解析成绝对路径,与进程 CWD 无关;WLD_OUTPUT_DIR 将来若改成绝对路径,
		// path 的 / 运算仍取绝对那一侧,这条候选继续成立。
		std::filesystem::path AnchoredGameDllPath()
		{
			return std::filesystem::path(WLD_REPO_ROOT) / WLD_OUTPUT_DIR / "bin" / WLD_BUILD_TYPE / "Game" / WLD_BUILD_TYPE / "Game.dll";
		}

		// 首选布局(打包/发布):exe 旁的 bin/ 下递归找 Game.dll。找不到返回空路径;
		// searchRoot 回传实际搜索的目录,供日志显示"试过哪里"。
		std::filesystem::path FindGameDllBesideExecutable(std::filesystem::path& searchRoot)
		{
			char exePathBuf[MAX_PATH];
			if (!GetModuleFileNameA(NULL, exePathBuf, MAX_PATH))
				return {};
			searchRoot = std::filesystem::path(exePathBuf).parent_path() / "bin";
			if (!std::filesystem::exists(searchRoot))
				return {};
			for (const auto& entry : std::filesystem::recursive_directory_iterator(searchRoot))
			{
				if (entry.is_regular_file() && entry.path().filename() == "Game.dll")
					return entry.path();
			}
			return {};
		}
	}

	bool GameModuleHost::LoadDefault(WorldContext& context, std::string* error)
	{
		// 候选按优先级排列:exe 旁(打包布局,既有首选)其次仓库根锚定(开发布局,与 CWD 无关)。
		std::filesystem::path besideExeSearchDir;
		const std::filesystem::path besideExe = FindGameDllBesideExecutable(besideExeSearchDir);
		const std::filesystem::path anchored = AnchoredGameDllPath();

		const std::string besideExeSearchText = besideExeSearchDir.empty() ? std::string("(exe path unavailable)") : besideExeSearchDir.string();
		const std::string besideExeText = besideExe.empty() ? std::string("(no Game.dll found)") : besideExe.string();
		WLD_CORE_INFO("[game-module] candidate 1 (beside exe): searched {0} -> {1}", besideExeSearchText, besideExeText);
		WLD_CORE_INFO("[game-module] candidate 2 (repo-root anchored): {0}", anchored.string());

		std::vector<std::filesystem::path> candidates;
		if (!besideExe.empty())
			candidates.push_back(besideExe);
		candidates.push_back(anchored);

		std::string failure;
		for (const std::filesystem::path& candidate : candidates)
		{
			std::string candidateError;
			if (context.Modules().Load(candidate, context, &candidateError) == ModuleManager::Status::Ok)
			{
				WLD_CORE_INFO("[game-module] loaded: {0}", candidate.string());
				return true;
			}
			WLD_CORE_WARN("[game-module] candidate failed: {0} ({1})", candidate.string(), candidateError);
			if (!failure.empty())
				failure += "; ";
			failure += candidate.string() + ": " + candidateError;
		}

		if (error)
			*error = failure;
		return false;
	}
}
