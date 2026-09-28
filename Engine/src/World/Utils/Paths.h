#pragma once

#include "World/Core/Export.h"

#include <filesystem>

namespace World
{
	// PROJ-1 P4:运行期"当前项目根 / 内容根"的唯一入口。
	//
	// 为什么要有它:项目根在改造前是编译期常量(WLD_PROJECT_DIR / WLD_ASSETPATH),
	// 于是"打开任意位置的项目"不可能 —— 资产/本地化/清单永远解析回 projects/default。
	// 解析优先级:
	//   SetProjectDirOverride()(--project 等宿主显式入口)
	//     > 环境变量 WLD_PROJECT_DIR(非空)
	//       > 编译期默认值(宏 WLD_PROJECT_DIR,即仓库的 projects/default/)。
	// 内容根默认 = ProjectDir()/"assets"(标准清单的 content_root: assets);
	// 只有单独 SetAssetRootOverride() 过的宿主,内容根才与项目根解耦。
	//
	// 约定:返回的是**进程级静态对象**的引用(整个进程一份状态,World 是 DLL 也一样),
	// 环境变量在首次调用时才读取(不受静态初始化顺序影响),读取与覆盖都是线程安全的。
	namespace Paths
	{
		// 当前项目根:绝对路径、已词法规范化、无尾分隔符。
		WLD_API const std::filesystem::path& ProjectDir();
		// 覆盖项目根(空路径 = 清除覆盖,回到环境变量/编译期默认)。
		WLD_API void SetProjectDirOverride(const std::filesystem::path& dir);
		// ProjectDir() / relative:替代 `std::string(WLD_PROJECT_DIR) + "x"` 的写法。
		WLD_API std::filesystem::path ProjectFile(const std::filesystem::path& relative);
		// 内容根(项目资产目录):默认 ProjectDir()/"assets"。
		WLD_API const std::filesystem::path& AssetRoot();
		// 覆盖内容根(空 = 清除)。一般不必单独设置:项目根一变就跟随。
		WLD_API void SetAssetRootOverride(const std::filesystem::path& dir);
	}
}
