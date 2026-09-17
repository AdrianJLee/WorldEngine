#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	struct LuaTypeReflection;

	namespace Schema { struct TypeSchema; }

	class LuaStubGenerator
	{
	public:
		// Pure rendering: neither the registry, output nor filesystem changes on failure.
		static bool Render(const std::vector<LuaTypeReflection>& types, std::string& output, std::string& error);

		// W3a-A2:除 Lua 类型外,把 schema 组件类型(来自 SchemaRegistry::List(TypeCategory::Component))
		// 渲染成注解块,追加在既有 Lua 类型块之后(既有块内容与顺序不变)。
		// 组件之间按短类名升序,字段按稳定 field id 升序;同一输入两次渲染逐字节一致。
		static bool Render(const std::vector<LuaTypeReflection>& types,
			const std::vector<const Schema::TypeSchema*>& components, std::string& output, std::string& error);

		// A successful unchanged generation leaves the destination timestamp intact.
		// A failed generation retains the last valid destination and reports its path.
		static bool Generate(const std::filesystem::path& outputPath, std::string& error);
		static bool Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types, std::string& error);
		static bool Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types,
			const std::vector<const Schema::TypeSchema*>& components, std::string& error);
	};
}
