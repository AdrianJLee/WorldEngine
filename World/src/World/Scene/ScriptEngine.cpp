#include "wldpch.h"
#include "ScriptEngine.h"
#include "Components.h"
#include "LuaStubGenerator.h"
#include "World/Core/Application.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"

#include <any>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace World
{
	std::vector<LuaTypeReflection>& LuaReflectionRegistry::GetTable()
	{
		static std::vector<LuaTypeReflection> s_Table;
		return s_Table;
	}

	namespace
	{
		std::unique_ptr<LuauVm> s_Vm;
		std::unique_ptr<ScriptBindingContext> s_Bindings;
		// 受保护的 `target[key]` 查找:脚本 __index 抛错时错误要变成可诊断文本,
		// 不能 longjmp 穿过还活着的 C++ 局部对象(与 T1 的表写限制同源)。
		ScriptFunctionRef s_LookupField;
		// 受保护的字段名收集:脚本返回的表可能有 __index 元表,只收集**自有字符串键**。
		ScriptFunctionRef s_CollectFieldNames;
		std::thread::id s_OwnerThread;

		const char* const kFieldLookupSource = "return function(target, key) return target[key] end";
		const char* const kFieldCollectorSource =
			"return function(target)\n"
			"    local names = {}\n"
			"    for key, _ in pairs(target) do\n"
			"        if type(key) == 'string' then table.insert(names, key) end\n"
			"    end\n"
			"    return names\n"
			"end";

		void ReportLuaError(LuaScriptComponent& script, const char* phase, const std::string& error)
		{
			const std::string message = "[Lua] " + script.ScriptFilePath + " entity=" +
				std::to_string(static_cast<uint32_t>(script.RuntimeEntity)) + " phase=" + phase + ":\n" + error;
			if (!script.LastError.empty()) script.LastError += "\n";
			script.LastError += message;
			script.State = ScriptInstanceState::Faulted;
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("{0}", message);
		}

		void ClearLuaReferences(LuaScriptComponent& script)
		{
			script.OnCreateFunc.Release();
			script.OnUpdateFunc.Release();
			script.OnDestroyFunc.Release();
			script.ScriptTable.Release();
			script.LuaEnv.Release();
			script.RuntimeEntity = {};
			script.IsLoaded = false;
			script.CreateEntered = false;
		}

		// VFS 优先读脚本源码;未命中回退磁盘;找不到抛 logic_error。
		std::string ReadScriptSource(const std::string& scriptFilePath)
		{
			if (Application::HasInstance())
			{
				std::error_code ec;
				std::vector<uint8_t> bytes;
				if (Application::Get().GetContext().Vfs().Read(scriptFilePath, bytes, ec) && !bytes.empty())
					return std::string(bytes.begin(), bytes.end());
			}
			const std::filesystem::path diskPath =
				WLD_ASSETPATH + std::string("/") + scriptFilePath;
			std::ifstream file(diskPath, std::ios::binary);
			if (file.is_open())
			{
				std::stringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			}
			throw std::logic_error("Script not found: " + scriptFilePath);
		}

		LuaTypeReflection VectorDescription(const char* name, size_t dimensions)
		{
			LuaTypeReflection type;
			type.ClassName = name;
			const char* axes[] = { "x", "y", "z", "w" };
			std::vector<LuaPropDesc> coordinates;
			for (size_t i = 0; i < dimensions; ++i)
			{
				type.Properties.push_back({ axes[i], "number", "Vector coordinate" });
				coordinates.push_back({ axes[i], "number", "Vector coordinate" });
			}
			type.Methods = { { "length", {}, "number", "Euclidean vector length", true } };
			type.Constructors = {
				{ "new", {}, name, "Create a vector", false },
				{ "new", {{ "value", "number", "Value for every coordinate" }}, name, "Fill every coordinate", false },
				{ "new", coordinates, name, "Create a vector from coordinates", false }
			};
			return type;
		}

		std::unordered_map<std::string, std::string> ParseFieldAnnotationsInternal(const std::string& text)
		{
			std::unordered_map<std::string, std::string> schema;
			std::istringstream stream(text);
			std::string line;
			while (std::getline(stream, line))
			{
				const size_t start = line.find_first_not_of(" \t\r\n");
				if (start == std::string::npos)
					continue;
				const std::string trimmed = line.substr(start);
				if (trimmed.rfind("---@field", 0) != 0)
					continue;
				std::istringstream rest(trimmed.substr(9));
				std::string name, type;
				if (!(rest >> name))
					continue;
				if (!(rest >> type))
					continue;
				schema[name] = type;
			}
			return schema;
		}

		LuaFieldType AnnotationToFieldTypeInternal(const std::string& typeName, const ScriptValue& value)
		{
			if (typeName == "number")
			{
				double number = 0.0;
				if (!value.AsNumber(&number))
					return LuaFieldType::None;
				if (std::isfinite(number) && number == std::floor(number) &&
					number >= (std::numeric_limits<int>::min)() && number <= (std::numeric_limits<int>::max)())
					return LuaFieldType::Int;
				return LuaFieldType::Float;
			}
			if (typeName == "integer" || typeName == "int")
				return value.IsNumber() ? LuaFieldType::Int : LuaFieldType::None;
			if (typeName == "boolean" || typeName == "bool")
				return value.IsBoolean() ? LuaFieldType::Bool : LuaFieldType::None;
			if (typeName == "string")
				return value.IsString() ? LuaFieldType::String : LuaFieldType::None;
			return LuaFieldType::None;
		}

		bool ExtractNumber(const ScriptValue& value, double* out)
		{
			double number = 0.0;
			if (!value.AsNumber(&number))
				return false;
			if (out)
				*out = number;
			return true;
		}

		// 脚本返回表 -> 内存字段缓存(键仍是字段名;同类型才复用旧值 —— 与 sol2 时期一致)。
		std::unordered_map<std::string, LuaScriptField> BuildFieldCache(const ScriptTableRef& table,
			const std::unordered_map<std::string, std::string>& schema, const LuaScriptComponent& script)
		{
			std::unordered_map<std::string, LuaScriptField> fields;
			std::vector<std::string> names;
			{
				ScriptValue namesValue;
				std::string error;
				const ScriptValue args[] = { table.ToValue() };
				if (!s_CollectFieldNames.Call(args, 1, &namesValue, &error))
					throw std::runtime_error(error);
				ScriptTableRef array;
				if (namesValue.AsTable(&array))
				{
					for (const ScriptValue& item : array.GetArray())
					{
						std::string name;
						if (item.AsString(&name))
							names.push_back(std::move(name));
					}
				}
			}

			for (const std::string& name : names)
			{
				if (name.empty() || name[0] == '_' || name == "entity")
					continue;
				const ScriptValue value = table.GetField(name.c_str());
				LuaScriptField field;
				const auto schemaIt = schema.find(name);
				if (schemaIt != schema.end())
				{
					field.Type = AnnotationToFieldTypeInternal(schemaIt->second, value);
					if (field.Type == LuaFieldType::None)
						continue;
					switch (field.Type)
					{
						case LuaFieldType::Int:
						{
							double number = 0.0;
							ExtractNumber(value, &number);
							field.Value = static_cast<int>(number);
							break;
						}
						case LuaFieldType::Float:
						{
							double number = 0.0;
							ExtractNumber(value, &number);
							field.Value = static_cast<float>(number);
							break;
						}
						case LuaFieldType::Bool:
						{
							bool boolean = false;
							value.AsBool(&boolean);
							field.Value = boolean;
							break;
						}
						case LuaFieldType::String:
						{
							std::string text;
							value.AsString(&text);
							field.Value = std::move(text);
							break;
						}
						default: break;
					}
				}
				else
				{
					double number = 0.0;
					bool boolean = false;
					std::string text;
					if (value.AsNumber(&number))
					{
						if (std::isfinite(number) && number == std::floor(number) &&
							number >= (std::numeric_limits<int>::min)() && number <= (std::numeric_limits<int>::max)())
							field = { LuaFieldType::Int, static_cast<int>(number) };
						else
							field = { LuaFieldType::Float, static_cast<float>(number) };
					}
					else if (value.AsBool(&boolean)) field = { LuaFieldType::Bool, boolean };
					else if (value.AsString(&text)) field = { LuaFieldType::String, std::move(text) };
				}
				if (field.Type == LuaFieldType::None)
					continue;
				const auto old = script.CachedFields.find(name);
				if (old != script.CachedFields.end() && old->second.Type == field.Type)
					field.Value = old->second.Value;
				fields.emplace(name, std::move(field));
			}
			return fields;
		}

		// 脚本表上的回调:走 __index 继承;非函数非 nil 视为加载错误。
		bool ReadCallback(const ScriptTableRef& table, const char* name, ScriptFunctionRef* out, std::string* error)
		{
			if (out)
				out->Release();
			const ScriptValue args[] = { table.ToValue(), ScriptValue::String(name) };
			ScriptValue value;
			if (!s_LookupField.Call(args, 2, &value, error))
				return false;
			if (value.IsNil())
				return true;
			if (!out || !value.AsFunction(out))
			{
				if (error)
					*error = std::string(name) + " must be a function or nil";
				return false;
			}
			return true;
		}

		ScriptValue MakeEntityValue(ScriptBindingContext& bindings, Entity entity)
		{
			ScriptValue value = bindings.NewUserdata("Entity");
			Entity* target = nullptr;
			if (!bindings.Unwrap("Entity", value, &target) || !target)
				throw std::logic_error("Entity user type is not registered");
			new (target) Entity(entity);
			return value;
		}

		void ApplyCachedFields(LuaScriptComponent& script)
		{
			for (const auto& [name, field] : script.CachedFields)
			{
				ScriptValue value;
				switch (field.Type)
				{
					case LuaFieldType::Float: value = ScriptValue::Number(std::any_cast<float>(field.Value)); break;
					case LuaFieldType::Int: value = ScriptValue::Number(std::any_cast<int>(field.Value)); break;
					case LuaFieldType::Bool: value = ScriptValue::Boolean(std::any_cast<bool>(field.Value)); break;
					case LuaFieldType::String: value = ScriptValue::String(std::any_cast<std::string>(field.Value)); break;
					default: continue;
				}
				if (!script.ScriptTable.SetField(name.c_str(), value))
					throw std::logic_error("Cannot assign script field '" + name + "'");
			}
		}

		// 编译并在**独立 environment** 里执行脚本,取出返回的表。
		// chunkName 用逻辑脚本路径:错误文本里的文件/行号要能被编辑器直接定位。
		ScriptTableRef InstantiateScriptTable(const std::string& source, const char* chunkName,
			const ScriptTableRef& environment)
		{
			ScriptTableRef table;
			std::string error;
			ScriptFunctionRef chunk = s_Vm->CompileFunction(source, chunkName, environment, &error);
			if (!chunk.IsValid())
				throw std::runtime_error(error.empty() ? "Script failed to compile" : error);
			ScriptValue result;
			if (!chunk.Call(nullptr, 0, &result, &error))
				throw std::runtime_error(error);
			if (!result.AsTable(&table))
				throw std::logic_error("Script must return a table");
			return table;
		}

		ScriptFunctionRef CompileHelper(const char* source, const char* chunkName, std::string* error)
		{
			ScriptFunctionRef chunk = s_Vm->CompileFunction(source, chunkName, ScriptTableRef{}, error);
			if (!chunk.IsValid())
				return {};
			ScriptValue result;
			if (!chunk.Call(nullptr, 0, &result, error))
				return {};
			ScriptFunctionRef function;
			if (!result.AsFunction(&function) && error)
				*error = std::string(chunkName) + ": helper chunk did not return a function";
			return function;
		}

		ScriptValue PrintImplementation(const ScriptValue* args, std::size_t argCount)
		{
			ScriptEngine::AssertOwnerThread();
			std::string text = "[Lua]";
			ScriptFunctionRef tostring;
			if (!s_Vm->GetGlobal("tostring").AsFunction(&tostring))
				throw std::runtime_error("tostring is not available");
			for (std::size_t index = 0; index < argCount; ++index)
			{
				ScriptValue converted;
				std::string error;
				if (!tostring.Call(&args[index], 1, &converted, &error))
					throw std::runtime_error(error);
				std::string piece;
				if (!converted.AsString(&piece))
					piece = "?";
				text += " " + piece;
			}
			if (Log::GetClientLogger()) WLD_TRACE("{0}", text);
			return ScriptValue::Nil();
		}

		void ShutdownInternal()
		{
			s_LookupField.Release();
			s_CollectFieldNames.Release();
			s_Bindings.reset();
			if (s_Vm)
				s_Vm->Shutdown();
			s_Vm.reset();
			s_OwnerThread = {};
		}
	}

	bool ScriptEngine::IsInitialized() { return s_Vm != nullptr; }

	void ScriptEngine::AssertOwnerThread()
	{
		if (!s_Vm) throw std::logic_error("ScriptEngine is not initialized");
		if (s_OwnerThread != std::this_thread::get_id()) throw std::logic_error("Lua access must run on the ScriptEngine owner thread");
	}

	void ScriptEngine::Init()
	{
		if (s_Vm) { AssertOwnerThread(); return; }
		s_OwnerThread = std::this_thread::get_id();
		std::string error;
		auto vm = std::make_unique<LuauVm>();
		if (!vm->Init(&error))
			throw std::runtime_error("[Lua] failed to initialize the Luau VM: " + error);
		s_Vm = std::move(vm);
		try
		{
			s_Bindings = std::make_unique<ScriptBindingContext>(*s_Vm);
			if (!s_Bindings->IsValid())
				throw std::runtime_error("[Lua] failed to create the script binding context");

			// 沙箱纪律(T1 坑 #1):print 替换与宿主注入必须发生在**编译脚本之前**,
			// 否则无 env 的 chunk 已在 load 期解析过 import,替换对已编译函数无效。
			if (!s_Vm->SetGlobal("print", s_Bindings->CreateFunction("print", &PrintImplementation)))
				throw std::runtime_error("[Lua] failed to install the print implementation");

			s_LookupField = CompileHelper(kFieldLookupSource, "WorldEngine.FieldLookup", &error);
			s_CollectFieldNames = CompileHelper(kFieldCollectorSource, "WorldEngine.FieldNames", &error);
			if (!s_LookupField.IsValid() || !s_CollectFieldNames.IsValid())
				throw std::runtime_error("[Lua] failed to create script helpers: " + error);

			RegisterMathTypes();
		}
		catch (...)
		{
			ShutdownInternal();
			throw;
		}
		if (Log::GetCoreLogger()) WLD_CORE_INFO("[Lua] ScriptEngine initialized successfully.");
	}

	void ScriptEngine::Shutdown()
	{
		if (!s_Vm) return;
		AssertOwnerThread();
		// Hosts release all scene/preview references before entering this function.
		ShutdownInternal();
	}

	LuauVm& ScriptEngine::GetState()
	{
		AssertOwnerThread();
		return *s_Vm;
	}

	ScriptBindingContext& ScriptEngine::GetBindingContext()
	{
		AssertOwnerThread();
		if (!s_Bindings) throw std::logic_error("ScriptEngine is not initialized");
		return *s_Bindings;
	}

	void ScriptEngine::DefineMathType()
	{
		if (IsInitialized()) AssertOwnerThread();
		auto vec2 = VectorDescription("vec2", 2);
		vec2.Operators = { { "add", "vec2", "vec2" }, { "sub", "vec2", "vec2" },
			{ "mul", "number", "vec2" }, { "mul", "vec2", "vec2" }, { "div", "number", "vec2" }, { "div", "vec2", "vec2" } };
		vec2.BindFunc = [](ScriptBindingContext& bindings) { RegisterBuiltinVec2Binding(bindings); };
		LuaReflectionRegistry::Register(vec2);

		auto vec3 = VectorDescription("vec3", 3);
		vec3.Methods.push_back({ "normalize", {}, "vec3", "Return a normalized vector", true });
		vec3.Methods.push_back({ "dot", {{ "other", "vec3", "Other vector" }}, "number", "Dot product", true });
		vec3.Methods.push_back({ "cross", {{ "other", "vec3", "Other vector" }}, "vec3", "Cross product", true });
		vec3.Operators = { { "add", "vec3", "vec3" }, { "sub", "vec3", "vec3" },
			{ "mul", "number", "vec3" }, { "mul", "vec3", "vec3" }, { "div", "number", "vec3" }, { "div", "vec3", "vec3" } };
		vec3.BindFunc = [](ScriptBindingContext& bindings) { RegisterBuiltinVec3Binding(bindings); };
		LuaReflectionRegistry::Register(vec3);

		auto vec4 = VectorDescription("vec4", 4);
		vec4.BindFunc = [](ScriptBindingContext& bindings) { RegisterBuiltinVec4Binding(bindings); };
		LuaReflectionRegistry::Register(vec4);
	}

	void ScriptEngine::RegisterMathTypes()
	{
		AssertOwnerThread();
		if (!s_Bindings) throw std::logic_error("ScriptEngine is not initialized");
		DefineMathType();
		RegisterBuiltinEntityLuaType();
		RegisterBuiltinMat3LuaType();
		RegisterBuiltinMat4LuaType();
		for (const auto& type : LuaReflectionRegistry::GetTable())
			if (type.BindFunc) type.BindFunc(*s_Bindings);
	}

	bool ScriptEngine::GenerateLuaStubs()
	{
		AssertOwnerThread();
		std::string error;
		const std::filesystem::path path(WLD_ASSETPATH + std::string("/scripts/intermediate/WorldEngineAPI.lua"));
		if (!LuaStubGenerator::Generate(path, error))
		{
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("[Lua] {0}", error);
			return false;
		}
		return true;
	}

	std::unordered_map<std::string, std::string> ScriptEngine::ParseFieldAnnotations(const std::string& scriptText)
	{
		return ParseFieldAnnotationsInternal(scriptText);
	}

	bool ScriptEngine::InitScriptForEditor(LuaScriptComponent& script)
	{
		AssertOwnerThread();
		if (script.ScriptFilePath.empty() || script.IsLoaded || script.State == ScriptInstanceState::Creating ||
			script.State == ScriptInstanceState::Running || script.State == ScriptInstanceState::Destroying) return false;
		try
		{
			// 1. 读取脚本源码，静态解析 ---@field 注解（不执行脚本顶层代码）。
			const std::string source = ReadScriptSource(script.ScriptFilePath);
			std::unordered_map<std::string, std::string> schema = ParseFieldAnnotationsInternal(source);

			// 2. 在独立 environment 里执行脚本，获取默认值表（不保留引用：编辑器预览只缓存字段）。
			ScriptTableRef environment = s_Vm->CreateEnvironment();
			if (!environment.IsValid()) throw std::logic_error("Cannot create a script environment");
			ScriptTableRef table = InstantiateScriptTable(source, script.ScriptFilePath.c_str(), environment);

			// 3. 构建字段：类型优先取注解，缺失注解走旧值推断兼容路径。
			script.CachedFields = BuildFieldCache(table, schema, script);
			script.LastError.clear();
			script.State = ScriptInstanceState::Stopped;
			return true;
		}
		catch (const std::exception& error) { script.LastError.clear(); ReportLuaError(script, "EditorLoad", error.what()); }
		catch (...) { script.LastError.clear(); ReportLuaError(script, "EditorLoad", "Unknown exception"); }
		return false;
	}

	void ScriptEngine::OnCreateScript(LuaScriptComponent& script, Entity entity)
	{
		AssertOwnerThread();
		if (script.State != ScriptInstanceState::Pending && script.State != ScriptInstanceState::Stopped) return;
		ClearLuaReferences(script);
		script.LastError.clear();
		script.RuntimeEntity = entity;
		if (script.ScriptFilePath.empty()) { script.State = ScriptInstanceState::Stopped; return; }
		script.State = ScriptInstanceState::Creating;
		const char* phase = "Load";
		try
		{
			const std::string source = ReadScriptSource(script.ScriptFilePath);
			ScriptTableRef environment = s_Vm->CreateEnvironment();
			if (!environment.IsValid()) throw std::logic_error("Cannot create a script environment");
			ScriptTableRef table = InstantiateScriptTable(source, script.ScriptFilePath.c_str(), environment);
			script.LuaEnv = environment;
			script.ScriptTable = table;

			ApplyCachedFields(script);
			// All names carry the same non-owning, generation-checked entity handle.
			const ScriptValue entityValue = MakeEntityValue(*s_Bindings, entity);
			if (!script.ScriptTable.SetField("entity", entityValue) ||
				!script.ScriptTable.SetField("__Entity", entityValue) ||
				!script.ScriptTable.SetField("__EntityID", entityValue))
				throw std::logic_error("Cannot assign the entity handle");

			std::string error;
			if (!ReadCallback(script.ScriptTable, "OnCreate", &script.OnCreateFunc, &error))
				throw std::runtime_error(error);
			if (!ReadCallback(script.ScriptTable, "OnUpdate", &script.OnUpdateFunc, &error))
				throw std::runtime_error(error);
			if (!ReadCallback(script.ScriptTable, "OnDestroy", &script.OnDestroyFunc, &error))
				throw std::runtime_error(error);

			script.IsLoaded = true;
			script.CreateEntered = true;
			phase = "OnCreate";
			if (script.OnCreateFunc.IsValid())
			{
				const ScriptValue args[] = { script.ScriptTable.ToValue() };
				if (!script.OnCreateFunc.Call(args, 1, nullptr, &error))
					throw std::runtime_error(error);
			}
			script.State = ScriptInstanceState::Running;
		}
		catch (const std::exception& error) { ReportLuaError(script, phase, error.what()); }
		catch (...) { ReportLuaError(script, phase, "Unknown exception"); }
	}

	void ScriptEngine::OnUpdateScript(LuaScriptComponent& script, Timestep ts)
	{
		AssertOwnerThread();
		if (script.State != ScriptInstanceState::Running || !script.IsLoaded) return;
		try
		{
			if (script.OnUpdateFunc.IsValid())
			{
				std::string error;
				const ScriptValue args[] = { script.ScriptTable.ToValue(), ScriptValue::Number(ts.GetSeconds()) };
				if (!script.OnUpdateFunc.Call(args, 2, nullptr, &error))
					throw std::runtime_error(error);
			}
		}
		catch (const std::exception& error) { ReportLuaError(script, "OnUpdate", error.what()); }
		catch (...) { ReportLuaError(script, "OnUpdate", "Unknown exception"); }
	}

	void ScriptEngine::OnDestroyScript(LuaScriptComponent& script)
	{
		// Empty/stopped components can outlive the VM; live references cannot.
		if (IsInitialized()) AssertOwnerThread();
		else if (script.IsLoaded || script.LuaEnv.IsValid() || script.ScriptTable.IsValid() ||
			script.OnCreateFunc.IsValid() || script.OnUpdateFunc.IsValid() || script.OnDestroyFunc.IsValid())
			throw std::logic_error("Script references must be released before ScriptEngine::Shutdown");
		if (script.State == ScriptInstanceState::Destroying) return;
		bool faulted = script.State == ScriptInstanceState::Faulted;
		script.State = ScriptInstanceState::Destroying;
		try
		{
			const bool entered = script.CreateEntered;
			script.CreateEntered = false;
			if (entered && script.OnDestroyFunc.IsValid())
			{
				std::string error;
				const ScriptValue args[] = { script.ScriptTable.ToValue() };
				if (!script.OnDestroyFunc.Call(args, 1, nullptr, &error))
					throw std::runtime_error(error);
			}
		}
		catch (const std::exception& error) { ReportLuaError(script, "OnDestroy", error.what()); faulted = true; }
		catch (...) { ReportLuaError(script, "OnDestroy", "Unknown exception"); faulted = true; }
		ClearLuaReferences(script);
		script.State = faulted ? ScriptInstanceState::Faulted : ScriptInstanceState::Stopped;
	}
}
