#pragma once

#include "World/Core/Export.h"

#include <string>
#include <string_view>

namespace World
{
	// P2 W9.7:轻量 Luau 格式化 —— 只做"缩进(按块结构)+ 去行尾空白",
	// 不重排运算符/换行,不改块注释与长字符串内部的原始内容。
	// 返回整段源码(行尾符保持原样:CRLF 行继续 CRLF)。
	WLD_API std::string FormatLuauSource(std::string_view source, int indentSize = 4);
}
