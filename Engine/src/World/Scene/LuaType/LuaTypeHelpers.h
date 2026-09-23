#pragma once

#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <cmath>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>

namespace World
{
	// LuaType/** 内部的参数校验/装箱助手:全部通过抛异常报错,由绑定层的 trampoline
	// 转成带脚本文件/行号的 Lua error(见 ScriptBindingContext.cpp)。
	namespace LuaTypeDetail
	{
		inline double RequireNumber(const ScriptValue& value, const char* what)
		{
			double number = 0.0;
			if (!value.AsNumber(&number))
				throw std::logic_error(std::string(what) + " expects a number");
			return number;
		}

		inline float RequireFloat(const ScriptValue& value, const char* what)
		{
			return static_cast<float>(RequireNumber(value, what));
		}

		inline std::string RequireString(const ScriptValue& value, const char* what)
		{
			std::string text;
			if (!value.AsString(&text))
				throw std::logic_error(std::string(what) + " expects a string");
			return text;
		}

		inline std::string RequireStringArgument(const ScriptValue* args, std::size_t argCount, std::size_t index, const char* what)
		{
			if (index >= argCount)
				throw std::logic_error(std::string(what) + " expects an argument");
			return RequireString(args[index], what);
		}

		template <typename T>
		inline T* RequireReceiver(ScriptBindingContext& bindings, const char* typeName,
			const ScriptValue* args, std::size_t argCount, const char* method)
		{
			T* self = nullptr;
			if (argCount < 1 || !bindings.Unwrap<T>(typeName, args[0], &self) || !self)
				throw std::logic_error(std::string(method) + " expects a " + typeName + " receiver");
			return self;
		}

		template <typename T>
		inline T* TryUnwrap(ScriptBindingContext& bindings, const char* typeName, const ScriptValue& value)
		{
			T* pointer = nullptr;
			bindings.Unwrap<T>(typeName, value, &pointer);
			return pointer;
		}

		template <typename T>
		inline ScriptValue NewUserdataOf(ScriptBindingContext& bindings, const char* typeName, const T& value)
		{
			ScriptValue result = bindings.NewUserdata(typeName);
			T* target = nullptr;
			if (!bindings.Unwrap<T>(typeName, result, &target) || !target)
				throw std::logic_error(std::string(typeName) + ": failed to allocate a result");
			new (target) T(value);
			return result;
		}

		// 矩阵列索引:越界/非有限数值报 Lua error;错误文本沿用 sol2 时期的 "列号越界" 口径。
		inline int RequireColumnIndex(const ScriptValue& value, int columnCount, const char* typeName)
		{
			double number = 0.0;
			if (!value.AsNumber(&number) || !std::isfinite(number))
				throw std::out_of_range(std::string(typeName) + " column index is outside [0, " + std::to_string(columnCount - 1) + "]");
			if (number < 0.0 || number >= static_cast<double>(columnCount))
			{
				std::ostringstream text;
				text << typeName << " column index " << number << " is outside [0, " << (columnCount - 1) << "]";
				throw std::out_of_range(text.str());
			}
			return static_cast<int>(number);
		}
	}
}
