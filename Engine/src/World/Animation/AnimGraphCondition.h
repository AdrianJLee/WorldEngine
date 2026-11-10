#pragma once

// .wanimgraph transition 条件表达式解析（无括号；&& 优先级高于 ||）。
// 语法:  Param op Number  (op ∈ > >= < <= == !=)，用 && / || 连接。
// 未知参数按 0 处理；解析失败返回空 function（调用方视为“永不触发”），不抛异常。

#include "World/Core/Export.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace World
{
	namespace AnimGraphCondition
	{
		using ParameterMap = std::unordered_map<std::string, float>;
		using Predicate = std::function<bool(const ParameterMap&)>;

		WLD_API Predicate Parse(std::string_view expression, std::string* error = nullptr);
	}
}
