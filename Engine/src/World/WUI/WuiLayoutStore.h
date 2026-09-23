#pragma once

#include "World/WUI/WuiDock.h"

#include <filesystem>
#include <string>

namespace World::Wui
{
	// 布局持久化:默认布局 + JSON 读写;损坏/缺失回退默认布局。
	class WuiLayoutStore
	{
	public:
		static bool Save(const std::filesystem::path& path, const DockLayout& layout, std::string* error);
		static bool Load(const std::filesystem::path& path, const DockLayout& fallback, DockLayout* out, std::string* error);
	};
}
