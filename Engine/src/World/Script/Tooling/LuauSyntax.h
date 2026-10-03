#pragma once

#include "World/Core/Export.h"

#include <string>
#include <string_view>

namespace World
{
	// P2 W9.7:轻量语法检查(只编译、不执行、不写容器)—— 编辑器防抖检查用。
	struct LuauSyntaxError
	{
		int Line = 0;          // 1-based;0 = 编译器没给出行号
		std::string Message;   // 编译器原文(已去掉 ":行号:" 前缀)
	};

	// true = 语法通过;false 时 error(可为 null)填第一条错误。
	WLD_API bool CheckLuauSyntax(std::string_view source, const char* chunkName,
		LuauSyntaxError* error);
}
