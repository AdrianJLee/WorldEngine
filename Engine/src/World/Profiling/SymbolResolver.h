#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <string>

namespace World::Profiling
{
	// ---------------------------------------------------------------------------
	// 调用点(Site)符号化:把采样到的返回地址还原成 模块!函数+偏移 [文件:行]。
	//
	// 用途:回答"**哪一行**在分配"(内存归因三段式的第三段)。
	// 只在转储时调用;解析结果缓存;`SuppressionScope` 用来把符号化自身的分配
	// 排除在归因之外(否则观察者会污染被观察对象)。
	// ---------------------------------------------------------------------------
	class WLD_API SymbolResolver
	{
	public:
		static std::string Describe(void* address);

		// 抑制标记:作用域内产生的分配不计入归因。
		static bool Suppressed() noexcept;

		class SuppressionScope
		{
		public:
			SuppressionScope() noexcept;
			~SuppressionScope();
			SuppressionScope(const SuppressionScope&) = delete;
			SuppressionScope& operator=(const SuppressionScope&) = delete;
		};
	};
}
