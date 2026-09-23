#include "wldpch.h"
#include "LuaStubGenerator.h"
#include "ScriptEngine.h"
#include "World/Schema/Schema.h"
#include "World/Script/BindComponentAccess.h"
#include "World/Script/BindServices.h"

#include <atomic>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace World
{
	namespace
	{
		bool Fail(std::string& error, const std::string& location, const std::string& message)
		{
			error = location + ": " + message;
			return false;
		}

		bool IsNameStart(char c)
		{
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
		}

		bool IsNameChar(char c)
		{
			return IsNameStart(c) || (c >= '0' && c <= '9');
		}

		bool IsIdentifier(const std::string& name)
		{
			static const std::set<std::string> keywords = {
				"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
				"goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
				"true", "until", "while"
			};
			return !name.empty() && IsNameStart(name.front())
				&& std::all_of(name.begin(), name.end(), IsNameChar) && keywords.count(name) == 0;
		}

		bool IsDescription(const std::string& description)
		{
			// Keep metadata on one comment line; do not let it inject Lua/annotations.
			const auto first = description.find_first_not_of(' ');
			return (first == std::string::npos || description[first] != '@')
				&& std::none_of(description.begin(), description.end(), [](unsigned char c) { return c < 32 || c == 127; });
		}

		// The descriptor contract uses named types, unions and arrays, not raw Lua code
		// or anonymous function strings. Functions have their own structured metadata.
		bool IsType(const std::string& type, const std::set<std::string>& knownTypes)
		{
			size_t position = 0;
			while (position < type.size())
			{
				const size_t start = position;
				if (!IsNameStart(type[position])) return false;
				while (position < type.size() && IsNameChar(type[position])) ++position;
				if (knownTypes.count(type.substr(start, position - start)) == 0) return false;
				while (position + 1 < type.size() && type[position] == '[' && type[position + 1] == ']') position += 2;
				if (position == type.size()) return true;
				if (type[position++] != '|' || position == type.size()) return false;
			}
			return false;
		}

		bool IsFieldName(const std::string& name, const std::set<std::string>& knownTypes)
		{
			if (IsIdentifier(name)) return true;
			return name.size() > 2 && name.front() == '[' && name.back() == ']'
				&& IsType(name.substr(1, name.size() - 2), knownTypes);
		}

		std::string SignatureKey(const LuaFunctionDesc& function)
		{
			std::string result;
			for (const auto& parameter : function.Parameters)
				result += parameter.LuaType + ";";
			return result;
		}

		using FunctionGroups = std::map<std::string, std::vector<const LuaFunctionDesc*>>;

		bool AddFunctions(const LuaTypeReflection& type, const std::vector<LuaFunctionDesc>& functions,
			bool constructors, const std::set<std::string>& knownTypes, const std::set<std::string>& fields,
			FunctionGroups& groups, std::string& error)
		{
			for (const auto& function : functions)
			{
				const auto location = type.ClassName + "." + function.Name;
				if (!IsIdentifier(function.Name)) return Fail(error, location, "invalid function name");
				if (fields.count(function.Name)) return Fail(error, location, "duplicate field/function member name");
				if (constructors && (function.IsMethod || function.Name != "new" || function.ReturnType != type.ClassName))
					return Fail(error, location, "constructors must describe static new(...) returning the bound class");
				if (!constructors && function.Name == "new") return Fail(error, location, "new belongs in Constructors");
				if (!IsDescription(function.Description)) return Fail(error, location, "description must be a single comment line");
				if (!function.ReturnType.empty() && !IsType(function.ReturnType, knownTypes))
					return Fail(error, location, "invalid or unregistered return type '" + function.ReturnType + "'");

				std::set<std::string> parameters;
				for (size_t i = 0; i < function.Parameters.size(); ++i)
				{
					const auto& parameter = function.Parameters[i];
					const auto parameterLocation = location + " parameter " + std::to_string(i + 1) + " ('" + parameter.Name + "')";
					if (!IsIdentifier(parameter.Name) || (function.IsMethod && parameter.Name == "self"))
						return Fail(error, parameterLocation, "missing/invalid name, or explicit self on a colon method");
					if (!parameters.insert(parameter.Name).second) return Fail(error, parameterLocation, "duplicate parameter name");
					if (!IsType(parameter.LuaType, knownTypes)) return Fail(error, parameterLocation, "invalid or unregistered type '" + parameter.LuaType + "'");
					if (!IsDescription(parameter.Description)) return Fail(error, parameterLocation, "description must be a single comment line");
				}

				auto& overloads = groups[function.Name];
				for (const auto* previous : overloads)
				{
					if (previous->IsMethod != function.IsMethod) return Fail(error, location, "cannot mix static and instance overloads");
					if (SignatureKey(*previous) == SignatureKey(function)) return Fail(error, location, "duplicate function overload");
				}
				overloads.push_back(&function);
			}
			return true;
		}

		void WriteDescription(std::ostringstream& output, const std::string& description)
		{
			if (!description.empty()) output << " " << description;
		}

		void WriteFunction(std::ostringstream& output, const std::string& className,
			std::vector<const LuaFunctionDesc*> overloads)
		{
			std::sort(overloads.begin(), overloads.end(), [](const auto* a, const auto* b)
			{
				if (a->Parameters.size() != b->Parameters.size()) return a->Parameters.size() < b->Parameters.size();
				return SignatureKey(*a) < SignatureKey(*b);
			});
			const auto& primary = *overloads.front();
			if (!primary.Description.empty()) output << "---" << primary.Description << "\n";
			// LuaLS overload signatures carry names/types; retain each overload's prose
			// as well so changing any documented binding detail refreshes the file.
			for (size_t i = 1; i < overloads.size(); ++i)
			{
				const auto& overload = *overloads[i];
				if (!overload.Description.empty())
				{
					output << "---Overload " << overload.Name << "(";
					for (size_t parameterIndex = 0; parameterIndex < overload.Parameters.size(); ++parameterIndex)
					{
						if (parameterIndex) output << ", ";
						output << overload.Parameters[parameterIndex].Name;
					}
					output << "): " << overload.Description << "\n";
				}
				for (const auto& parameter : overload.Parameters)
					if (!parameter.Description.empty()) output << "---" << parameter.Name << ": " << parameter.Description << "\n";
			}
			for (size_t i = 1; i < overloads.size(); ++i)
			{
				const auto& overload = *overloads[i];
				output << "---@overload fun(";
				bool first = true;
				if (overload.IsMethod)
				{
					output << "self: " << className;
					first = false;
				}
				for (const auto& parameter : overload.Parameters)
				{
					if (!first) output << ", ";
					output << parameter.Name << ": " << parameter.LuaType;
					first = false;
				}
				output << ")";
				if (!overload.ReturnType.empty()) output << ": " << overload.ReturnType;
				output << "\n";
			}
			for (const auto& parameter : primary.Parameters)
			{
				output << "---@param " << parameter.Name << " " << parameter.LuaType;
				WriteDescription(output, parameter.Description);
				output << "\n";
			}
			if (!primary.ReturnType.empty()) output << "---@return " << primary.ReturnType << "\n";
			output << "function " << className << (primary.IsMethod ? ":" : ".") << primary.Name << "(";
			for (size_t i = 0; i < primary.Parameters.size(); ++i)
			{
				if (i) output << ", ";
				output << primary.Parameters[i].Name;
			}
			output << ") end\n\n";
		}

		bool WriteAtomically(const std::filesystem::path& path, const std::string& content, std::string& error)
		{
#ifdef _WIN32
			static std::atomic<unsigned long long> sequence { 0 };
			std::filesystem::path temporary;
			HANDLE file = INVALID_HANDLE_VALUE;
			for (unsigned attempt = 0; attempt < 64; ++attempt)
			{
				temporary = path;
				temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(sequence.fetch_add(1));
				file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file != INVALID_HANDLE_VALUE) break;
				const DWORD code = GetLastError();
				if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
					return Fail(error, path.u8string(), "cannot create temporary file: " + std::system_category().message(code));
			}
			if (file == INVALID_HANDLE_VALUE) return Fail(error, path.u8string(), "temporary filename collision limit reached");

			DWORD failure = ERROR_SUCCESS;
			size_t written = 0;
			while (written < content.size())
			{
				const DWORD count = static_cast<DWORD>((std::min)(content.size() - written, size_t { 1024 * 1024 }));
				DWORD actual = 0;
				if (!WriteFile(file, content.data() + written, count, &actual, nullptr)) { failure = GetLastError(); break; }
				if (actual == 0) { failure = ERROR_WRITE_FAULT; break; }
				written += actual;
			}
			if (failure == ERROR_SUCCESS && !FlushFileBuffers(file)) failure = GetLastError();
			if (!CloseHandle(file) && failure == ERROR_SUCCESS) failure = GetLastError();
			if (failure == ERROR_SUCCESS && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				failure = GetLastError();
			if (failure != ERROR_SUCCESS)
			{
				DeleteFileW(temporary.c_str());
				return Fail(error, path.u8string(), "atomic write failed: " + std::system_category().message(failure));
			}
			return true;
#else
			return Fail(error, path.u8string(), "atomic stub replacement requires the Windows platform implementation");
#endif
		}

		// W3a-A2:组件的脚本可见短名 = TypeId 最后一个 ':' 之后;Id.Name 为空时回退 DisplayName。
		std::string ComponentClassName(const Schema::TypeSchema& component)
		{
			const std::string& fullName = component.Id.Name;
			const size_t separator = fullName.find_last_of(':');
			if (separator != std::string::npos)
				return fullName.substr(separator + 1);
			return fullName.empty() ? component.DisplayName : fullName;
		}

		// 组件块 = 一行访问说明注释 + `---@class <短名>` + 每字段一行 `---@field <名字> <Lua 类型> <标注>`。
		// 字段顺序固定为 field id 升序(id 相同再按名字),与注册顺序/声明顺序无关。
		bool WriteComponentBlock(std::ostringstream& output, const Schema::TypeSchema& component,
			const std::set<std::string>& knownTypes, std::string& error)
		{
			const std::string name = ComponentClassName(component);
			std::vector<const Schema::FieldSchema*> fields;
			fields.reserve(component.Fields.size());
			for (const auto& field : component.Fields) fields.push_back(&field);
			std::stable_sort(fields.begin(), fields.end(), [](const Schema::FieldSchema* left, const Schema::FieldSchema* right)
			{
				if (left->Id.Value != right->Id.Value) return left->Id.Value < right->Id.Value;
				return left->Name < right->Name;
			});

			std::set<std::string> names;
			std::ostringstream block;
			block << "-- Component fields are exposed through Entity:GetComponent(\"" << name
				<< "\"); there is no runtime global named " << name << ".\n";
			block << "---@class " << name << "\n";
			for (const auto* field : fields)
			{
				const auto location = name + "." + field->Name;
				if (!IsIdentifier(field->Name)) return Fail(error, location, "invalid field name");
				if (!names.insert(field->Name).second) return Fail(error, location, "duplicate field name");
				// 类型名只从 W3a-A1 的映射表派生;未映射 Kind 用 "unknown" 占位 + 原因标注。
				const ScriptFieldAnnotation annotation = DescribeScriptFieldAnnotation(*field);
				if (!IsType(annotation.LuaType, knownTypes))
					return Fail(error, location, "invalid or unregistered type '" + annotation.LuaType + "'");
				block << "---@field " << field->Name << " " << annotation.LuaType;
				if (!annotation.Note.empty()) block << " " << annotation.Note;
				block << "\n";
			}
			block << "\n";
			output << block.str();
			return true;
		}

		// W3b:服务表(Input/Level/Save)的存根块。类注解行 + 每个方法一段
		// `---@param/---@return` + `function <表>:<方法>(...) end`,与既有类型块同一风格。
		bool WriteServiceBlock(std::ostringstream& output, const ScriptServiceBinding& service,
			const std::set<std::string>& knownTypes, const char* heading, std::string& error)
		{
			const std::string name = service.Name ? service.Name : "";
			if (!IsIdentifier(name))
				return Fail(error, "service table", "invalid or missing table name '" + name + "'");
			if (!IsDescription(service.Description ? service.Description : ""))
				return Fail(error, name, "description must be a single comment line");
			if (!service.Methods && service.MethodCount > 0)
				return Fail(error, name, "service table has no method descriptors");

			std::set<std::string> methodNames;
			std::ostringstream block;
			block << "-- " << (heading ? heading : "Global table") << " '" << name
				<< "': read-only; there is no runtime constructor.\n";
			if (service.Description && service.Description[0])
				block << "---" << service.Description << "\n";
			block << "---@class " << name << "\n"
				<< name << " = {}\n\n";
			for (std::size_t index = 0; index < service.MethodCount; ++index)
			{
				const ScriptServiceMethod& method = service.Methods[index];
				const std::string location = name + "." + (method.Name ? method.Name : std::string("<null>"));
				if (!method.Name || !IsIdentifier(method.Name))
					return Fail(error, location, "invalid function name");
				if (!methodNames.insert(method.Name).second)
					return Fail(error, location, "duplicate function name");
				if (!IsDescription(method.Description ? method.Description : ""))
					return Fail(error, location, "description must be a single comment line");
				if (!method.Function)
					return Fail(error, location, "method has no implementation");
				if (method.ExpectedArgs > method.ParamCount)
					return Fail(error, location, "expected argument count exceeds the parameter descriptor");

				bool sawOptional = false;
				std::size_t required = 0;
				for (std::size_t paramIndex = 0; paramIndex < method.ParamCount; ++paramIndex)
				{
					const ScriptServiceParam& parameter = method.Params[paramIndex];
					if (!parameter.Name || !IsIdentifier(parameter.Name))
						return Fail(error, location, "parameter " + std::to_string(paramIndex + 1) + " has an invalid name");
					if (!parameter.LuaType || !IsType(parameter.LuaType, knownTypes))
						return Fail(error, location, "parameter '" + std::string(parameter.Name) +
							"' has an invalid or unregistered type");
					if (!IsDescription(parameter.Description ? parameter.Description : ""))
						return Fail(error, location, "parameter '" + std::string(parameter.Name) +
							"' description must be a single comment line");
					if (parameter.Required)
					{
						if (sawOptional)
							return Fail(error, location, "required parameter '" + std::string(parameter.Name) +
								"' follows an optional parameter");
						++required;
					}
					else
					{
						sawOptional = true;
					}
				}
				if (required > method.ExpectedArgs)
					return Fail(error, location, "required parameter count exceeds the expected argument count");
				if (!method.ReturnType || (method.ReturnType[0] && !IsType(method.ReturnType, knownTypes)))
					return Fail(error, location, "invalid or unregistered return type");

				if (method.Description && method.Description[0])
					block << "---" << method.Description << "\n";
				for (std::size_t paramIndex = 0; paramIndex < method.ParamCount; ++paramIndex)
				{
					const ScriptServiceParam& parameter = method.Params[paramIndex];
					block << "---@param " << parameter.Name << " " << parameter.LuaType;
					if (!parameter.Required) block << "?";
					WriteDescription(block, parameter.Description ? parameter.Description : "");
					block << "\n";
				}
				if (method.ReturnType && method.ReturnType[0])
					block << "---@return " << method.ReturnType << "\n";
				block << "function " << name << ":" << method.Name << "(";
				for (std::size_t paramIndex = 0; paramIndex < method.ParamCount; ++paramIndex)
				{
					if (paramIndex) block << ", ";
					block << method.Params[paramIndex].Name;
				}
				block << ") end\n\n";
			}
			output << block.str();
			return true;
		}
	}

	bool LuaStubGenerator::Render(const std::vector<LuaTypeReflection>& types, std::string& output, std::string& error)
	{
		static const std::vector<const Schema::TypeSchema*> noComponents;
		return Render(types, noComponents, output, error);
	}

	bool LuaStubGenerator::Render(const std::vector<LuaTypeReflection>& types,
		const std::vector<const Schema::TypeSchema*>& components, std::string& output, std::string& error)
	{
		static const std::vector<const ScriptServiceBinding*> noServices;
		return Render(types, components, noServices, output, error);
	}

	bool LuaStubGenerator::Render(const std::vector<LuaTypeReflection>& types,
		const std::vector<const Schema::TypeSchema*>& components,
		const std::vector<const ScriptServiceBinding*>& services, std::string& output, std::string& error)
	{
		static const std::vector<const ScriptServiceBinding*> noUiTables;
		return Render(types, components, services, noUiTables, output, error);
	}

	bool LuaStubGenerator::Render(const std::vector<LuaTypeReflection>& types,
		const std::vector<const Schema::TypeSchema*>& components,
		const std::vector<const ScriptServiceBinding*>& services,
		const std::vector<const ScriptServiceBinding*>& uiTables, std::string& output, std::string& error)
	{
		error.clear();
		std::set<std::string> knownTypes { "any", "unknown", "nil", "boolean", "number", "integer", "string", "table", "function", "thread", "userdata" };
		std::map<std::string, const LuaTypeReflection*> sortedTypes;
		for (const auto& type : types)
		{
			if (sortedTypes.count(type.ClassName)) return Fail(error, type.ClassName, "duplicate class name");
			if (!IsIdentifier(type.ClassName) || type.ClassName == "WorldScript" || knownTypes.count(type.ClassName))
				return Fail(error, "Lua type '" + type.ClassName + "'", "invalid or reserved class name");
			sortedTypes.emplace(type.ClassName, &type);
			knownTypes.insert(type.ClassName);
		}
		if (sortedTypes.count("Entity")) knownTypes.insert("WorldScript");

		// 组件类名先整体校验与登记,再渲染:与既有 Lua 类型同池查重,重名/保留名直接报错,不静默丢块。
		std::map<std::string, const Schema::TypeSchema*> sortedComponents;
		for (const auto* component : components)
		{
			if (!component) return Fail(error, "schema components", "null TypeSchema entry");
			const std::string name = ComponentClassName(*component);
			const auto location = "schema component '" + component->Id.Name + "'";
			if (!IsIdentifier(name) || name == "WorldScript")
				return Fail(error, location, "invalid or reserved class name '" + name + "'");
			if (knownTypes.count(name))
				return Fail(error, location, "class name '" + name + "' collides with a bound Lua type");
			if (!sortedComponents.emplace(name, component).second)
				return Fail(error, location, "duplicate component class name '" + name + "'");
		}
		for (const auto& entry : sortedComponents) knownTypes.insert(entry.first);

		// W3b:服务表先整体校验(名字/方法/参数/返回类型)再渲染,失败不留半截输出。
		for (const ScriptServiceBinding* service : services)
		{
			if (!service) return Fail(error, "script services", "null service binding entry");
			const std::string name = service->Name ? service->Name : "";
			if (!IsIdentifier(name) || name == "WorldScript")
				return Fail(error, "service table", "invalid or reserved class name '" + name + "'");
			if (knownTypes.count(name))
				return Fail(error, "service table '" + name + "'", "class name collides with a bound Lua type");
		}
		// W3c:UI 表与 Lua 类型/服务表同池校验;存根里不能出现两个同名全局表。
		std::set<std::string> globalTableNames;
		for (const ScriptServiceBinding* service : services)
			globalTableNames.insert(service->Name ? service->Name : "");
		for (const ScriptServiceBinding* table : uiTables)
		{
			if (!table) return Fail(error, "script UI tables", "null UI binding entry");
			const std::string name = table->Name ? table->Name : "";
			if (!IsIdentifier(name) || name == "WorldScript")
				return Fail(error, "script UI table", "invalid or reserved class name '" + name + "'");
			if (knownTypes.count(name))
				return Fail(error, "script UI table '" + name + "'", "class name collides with a bound Lua type");
			if (!globalTableNames.insert(name).second)
				return Fail(error, "script UI table '" + name + "'", "global table name is already in use");
		}

		std::ostringstream rendered;
		rendered << "---@meta\n\n-- Generated by WorldEngine LuaStubGenerator. Do not edit.\n"
			<< "-- Language-server declarations only; do not require or execute this file.\n\n";
		for (const auto& [name, type] : sortedTypes)
		{
			std::map<std::string, const LuaPropDesc*> sortedFields;
			std::set<std::string> fields;
			for (const auto& property : type->Properties)
			{
				const auto location = name + "." + property.Name;
				if (!IsFieldName(property.Name, knownTypes)) return Fail(error, location, "invalid field name");
				if (!fields.insert(property.Name).second) return Fail(error, location, "duplicate field name");
				if (!IsType(property.LuaType, knownTypes)) return Fail(error, location, "invalid or unregistered type '" + property.LuaType + "'");
				if (!IsDescription(property.Description)) return Fail(error, location, "description must be a single comment line");
				sortedFields.emplace(property.Name, &property);
			}
			FunctionGroups groups;
			if (!AddFunctions(*type, type->Methods, false, knownTypes, fields, groups, error)
				|| !AddFunctions(*type, type->Constructors, true, knownTypes, fields, groups, error)) return false;

			static const std::set<std::string> binaryOperators { "add", "sub", "mul", "div", "idiv", "mod", "pow", "concat", "band", "bor", "bxor", "shl", "shr", "eq", "lt", "le" };
			static const std::set<std::string> unaryOperators { "unm", "len", "bnot" };
			std::map<std::string, const LuaOperatorDesc*> sortedOperators;
			for (const auto& operation : type->Operators)
			{
				const auto location = name + " operator " + operation.Name;
				if ((!binaryOperators.count(operation.Name) || !IsType(operation.OperandType, knownTypes))
					&& (!unaryOperators.count(operation.Name) || !operation.OperandType.empty()))
					return Fail(error, location, "unsupported operator or invalid operand type");
				if (!IsType(operation.ReturnType, knownTypes)) return Fail(error, location, "invalid or unregistered return type");
				if (!sortedOperators.emplace(operation.Name + ":" + operation.OperandType, &operation).second)
					return Fail(error, location, "duplicate operator overload");
			}

			rendered << "---@class " << name << "\n";
			for (const auto& [fieldName, property] : sortedFields)
			{
				rendered << "---@field " << fieldName << " " << property->LuaType;
				WriteDescription(rendered, property->Description);
				rendered << "\n";
			}
			for (const auto& [key, operation] : sortedOperators)
			{
				rendered << "---@operator " << operation->Name;
				if (!operation->OperandType.empty()) rendered << "(" << operation->OperandType << ")";
				rendered << ": " << operation->ReturnType << "\n";
			}
			rendered << name << " = {}\n\n";
			for (const auto& [functionName, overloads] : groups) WriteFunction(rendered, name, overloads);
		}
		// W3b:服务块在既有 Lua 类型块之后、schema 组件块之前(组件块区段保持连续)。
		for (const ScriptServiceBinding* service : services)
			if (!WriteServiceBlock(rendered, *service, knownTypes, "Global service table", error)) return false;
		// W3c:UI 块紧随服务块之后、组件块之前。
		for (const ScriptServiceBinding* table : uiTables)
			if (!WriteServiceBlock(rendered, *table, knownTypes, "Global script UI table", error)) return false;
		// W3a-A2:组件块追加在既有 Lua 类型块之后(既有块内容与顺序不变),组件之间按短名升序。
		for (const auto& [name, component] : sortedComponents)
			if (!WriteComponentBlock(rendered, *component, knownTypes, error)) return false;
		if (sortedTypes.count("Entity"))
		{
			rendered << "---Annotation-only shape of a script table; there is no WorldScript runtime global or constructor.\n"
				<< "---@class WorldScript\n"
				<< "---@field entity Entity Entity owning this script instance.\n"
				<< "---@field OnCreate? fun(self: WorldScript) Called once when the instance starts.\n"
				<< "---@field OnUpdate? fun(self: WorldScript, dt: number) Updated on the scene thread; dt is elapsed seconds.\n"
				<< "---@field OnUI? fun(self: WorldScript) Called once per UI frame while the instance is running.\n"
				<< "---@field OnDestroy? fun(self: WorldScript) Cleanup callback, attempted at most once after creation begins.\n";
		}
		output = rendered.str();
		return true;
	}

	bool LuaStubGenerator::Generate(const std::filesystem::path& outputPath, std::string& error)
	{
		return Generate(outputPath, LuaReflectionRegistry::GetTable(), error);
	}

	bool LuaStubGenerator::Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types, std::string& error)
	{
		static const std::vector<const Schema::TypeSchema*> noComponents;
		return Generate(outputPath, types, noComponents, error);
	}

	bool LuaStubGenerator::Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types,
		const std::vector<const Schema::TypeSchema*>& components, std::string& error)
	{
		static const std::vector<const ScriptServiceBinding*> noServices;
		return Generate(outputPath, types, components, noServices, error);
	}

	bool LuaStubGenerator::Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types,
		const std::vector<const Schema::TypeSchema*>& components,
		const std::vector<const ScriptServiceBinding*>& services, std::string& error)
	{
		static const std::vector<const ScriptServiceBinding*> noUiTables;
		return Generate(outputPath, types, components, services, noUiTables, error);
	}

	bool LuaStubGenerator::Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types,
		const std::vector<const Schema::TypeSchema*>& components,
		const std::vector<const ScriptServiceBinding*>& services,
		const std::vector<const ScriptServiceBinding*>& uiTables, std::string& error)
	{
		std::string content;
		if (!Render(types, components, services, uiTables, content, error))
		{
			error = outputPath.u8string() + ": " + error;
			return false;
		}
		try
		{
			if (outputPath.empty() || outputPath.filename().empty()) return Fail(error, outputPath.u8string(), "missing output filename");
			if (std::filesystem::exists(outputPath))
			{
				if (!std::filesystem::is_regular_file(outputPath)) return Fail(error, outputPath.u8string(), "destination is not a regular file");
				std::ifstream existing(outputPath, std::ios::binary);
				if (!existing) return Fail(error, outputPath.u8string(), "cannot read existing declarations");
				const std::string previous((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
				if (existing.bad()) return Fail(error, outputPath.u8string(), "failed reading existing declarations");
				if (previous == content) return true;
			}
			if (!outputPath.parent_path().empty()) std::filesystem::create_directories(outputPath.parent_path());
			return WriteAtomically(outputPath, content, error);
		}
		catch (const std::exception& exception)
		{
			return Fail(error, outputPath.u8string(), exception.what());
		}
	}
}
