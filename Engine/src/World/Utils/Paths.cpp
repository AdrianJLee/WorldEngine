#include "wldpch.h"

#include "World/Utils/Paths.h"

#include <cstdlib>
#include <mutex>
#include <system_error>

namespace World::Paths
{
	namespace
	{
		struct PathsState
		{
			std::mutex Mutex;
			bool EnvironmentRead = false;
			std::filesystem::path EnvironmentProjectDir;   // WLD_PROJECT_DIR(空 = 环境变量未设置/为空)
			std::filesystem::path ProjectOverride;         // SetProjectDirOverride(空 = 无覆盖)
			std::filesystem::path AssetOverride;           // SetAssetRootOverride(空 = 无覆盖)
			bool ProjectResolved = false;
			std::filesystem::path ProjectDir;              // 解析结果:ProjectDir() 返回它的引用
			bool AssetResolved = false;
			std::filesystem::path AssetRoot;               // 解析结果:AssetRoot() 返回它的引用
		};

		PathsState& State()
		{
			// 函数内静态:首次调用才构造 —— 既绕开跨 TU/跨 DLL 的静态初始化顺序问题,
			// 也保证整个进程只有一份状态(World 以共享库形式被 Editor/Runtime/Game 共享)。
			static PathsState state;
			return state;
		}

		// 目录路径规范化:相对路径按当前工作目录补成绝对、词法规范化、去掉尾分隔符。
		// 刻意不做 canonical():那要求路径已存在,而覆盖可以指向尚未建好的目录。
		std::filesystem::path NormalizeDirectory(const std::filesystem::path& dir)
		{
			if (dir.empty())
				return {};

			std::error_code error;
			std::filesystem::path absolute = dir.is_absolute() ? dir : std::filesystem::absolute(dir, error);
			if (error)
				absolute = dir;   // absolute() 极罕见地失败时保留原值,不引入新的失败分支

			std::filesystem::path normalized = absolute.lexically_normal();
			// 去尾分隔符("E:/p/" → "E:/p");盘根("E:/"、"/")保持原样。
			while (!normalized.has_filename() && normalized != normalized.root_path())
				normalized = normalized.parent_path();
			return normalized;
		}

		std::filesystem::path CompileTimeProjectDir()
		{
			// 宏是唯一读取点:根 CMake 的 WLD_PROJECT_DIR = <repo>/projects/default/(带尾分隔符)。
			return NormalizeDirectory(std::filesystem::path(WLD_PROJECT_DIR));
		}

		// 解析当前项目根(幂等:只在失效后重算一次)。调用方必须已持有 Mutex。
		void ResolveProjectLocked(PathsState& state)
		{
			if (!state.EnvironmentRead)
			{
				state.EnvironmentRead = true;
				if (const char* value = std::getenv("WLD_PROJECT_DIR"))
					if (value[0] != '\0')
						state.EnvironmentProjectDir = NormalizeDirectory(std::filesystem::path(value));
			}
			if (state.ProjectResolved)
				return;

			if (!state.ProjectOverride.empty())
				state.ProjectDir = state.ProjectOverride;
			else if (!state.EnvironmentProjectDir.empty())
				state.ProjectDir = state.EnvironmentProjectDir;
			else
				state.ProjectDir = CompileTimeProjectDir();
			state.ProjectResolved = true;
		}
	}

	const std::filesystem::path& ProjectDir()
	{
		PathsState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);
		ResolveProjectLocked(state);
		return state.ProjectDir;
	}

	void SetProjectDirOverride(const std::filesystem::path& dir)
	{
		PathsState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);
		state.ProjectOverride = NormalizeDirectory(dir);
		state.ProjectResolved = false;
		state.AssetResolved = false;   // 项目根一变,默认内容根要跟着重算
	}

	std::filesystem::path ProjectFile(const std::filesystem::path& relative)
	{
		return ProjectDir() / relative;
	}

	const std::filesystem::path& AssetRoot()
	{
		PathsState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);
		ResolveProjectLocked(state);
		if (!state.AssetResolved)
		{
			state.AssetRoot = state.AssetOverride.empty()
				? state.ProjectDir / "assets"
				: state.AssetOverride;
			state.AssetResolved = true;
		}
		return state.AssetRoot;
	}

	void SetAssetRootOverride(const std::filesystem::path& dir)
	{
		PathsState& state = State();
		std::lock_guard<std::mutex> lock(state.Mutex);
		state.AssetOverride = NormalizeDirectory(dir);
		state.AssetResolved = false;
	}
}
