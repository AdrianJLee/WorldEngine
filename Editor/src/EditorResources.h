#pragma once

#include <filesystem>
#include <string>

namespace World
{
	// 编辑器自带资源(图标 / 字体 / 本地化 …)的**唯一**根 = 编辑器目录。
	//
	// P4-U12:引擎里只有两个根,各自唯一、互不回退 —— 游戏资产相对项目内容根(WLD_ASSETPATH),
	// 编辑器自己的资源相对 Editor/。「先内容根、再 Game/、再 Editor/」那种多候选会让
	// "路径写错却恰好命中另一个根"变成静默成功(图标挂掉一个多月没人发现的那种)。
	inline std::string EditorResourcePath(const char* relativePath)
	{
		return (std::filesystem::path(std::string(WLD_EDITOR_DIR)) / relativePath).string();
	}
}
