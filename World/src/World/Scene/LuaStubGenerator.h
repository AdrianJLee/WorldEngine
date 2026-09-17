#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	struct LuaTypeReflection;

	namespace Schema { struct TypeSchema; }

	// P2 W3b:服务绑定描述(BindServices.h)。存根里服务块的渲染也消费同一份描述,
	// 所以这里只前置声明,不在本文件里制造新的映射表。
	struct ScriptServiceBinding;

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

		// W3b:把只读服务表(Input/Level/Save)渲染成注解块,追加在既有 Lua 类型块之后、
		// schema 组件块之前。服务块内部按描述顺序输出,同一输入两次渲染逐字节一致。
		static bool Render(const std::vector<LuaTypeReflection>& types,
			const std::vector<const Schema::TypeSchema*>& components,
			const std::vector<const ScriptServiceBinding*>& services,
			std::string& output, std::string& error);

		// W3c:把脚本 UI 表(ui)渲染成注解块,追加在服务块之后、schema 组件块之前。
		static bool Render(const std::vector<LuaTypeReflection>& types,
			const std::vector<const Schema::TypeSchema*>& components,
			const std::vector<const ScriptServiceBinding*>& services,
			const std::vector<const ScriptServiceBinding*>& uiTables,
			std::string& output, std::string& error);

		// A successful unchanged generation leaves the destination timestamp intact.
		// A failed generation retains the last valid destination and reports its path.
		static bool Generate(const std::filesystem::path& outputPath, std::string& error);
		static bool Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types, std::string& error);
		static bool Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types,
			const std::vector<const Schema::TypeSchema*>& components, std::string& error);
		static bool Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types,
			const std::vector<const Schema::TypeSchema*>& components,
			const std::vector<const ScriptServiceBinding*>& services, std::string& error);
		static bool Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types,
			const std::vector<const Schema::TypeSchema*>& components,
			const std::vector<const ScriptServiceBinding*>& services,
			const std::vector<const ScriptServiceBinding*>& uiTables, std::string& error);
	};
}
