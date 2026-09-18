#include "wldpch.h"
#include "ScriptEngine.h"
#include "Components.h"
#include "LuaStubGenerator.h"
#include "World/Core/Application.h"
#include "World/Core/Vfs/Vfs.h"
#include "World/Core/Asset/ScriptArtifact.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Script/BehaviorRegistry.h"
#include "World/Script/BindEvents.h"
#include "World/Script/BindUI.h"
#include "World/Script/BindServices.h"
#include "World/Script/HotReload.h"
#include "World/Script/LuauVm.h"
#include "World/Script/Sandbox.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/WUI/WuiContext.h"

#include <any>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

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
		// W7-3:登记的内容上下文(宿主/U3 注入)。null = 回退 Application::HasInstance()。
		// 只保存指针、不拥有;Shutdown() 清空,宿主不需要反登记。
		WorldContext* s_ContentContext = nullptr;
		// W6:引擎默认预算(指令 1e6;时间关)。LuauVm 层保持 0 = 不限,
		// 避免改变 World.LuauVm / World.LuauBinding 的既有语义。
		Sandbox::Policy s_SandboxPolicy{ 1000000, 0 };
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
			script.OnUiFunc.Release();
			script.ScriptTable.Release();
			script.LuaEnv.Release();
			script.RuntimeEntity = {};
			script.IsLoaded = false;
			script.CreateEntered = false;
		}

		// W7-3:脚本读取的单一入口(二进制安全,容器字节里的 '\0' 原样保留)。
		// 顺序:登记的内容上下文 VFS(未登记时用既有 Application VFS)→ 磁盘
		// WLD_ASSETPATH/<逻辑路径>;两者都没命中抛可读错误(文本与既有 ReadScriptSource 一致)。
		std::vector<uint8_t> ReadScriptBytes(const std::string& scriptFilePath)
		{
			const Vfs::Vfs* vfs = nullptr;
			if (s_ContentContext)
				vfs = &s_ContentContext->Vfs();          // U3:headless 只挂包 provider
			else if (Application::HasInstance())
				vfs = &Application::Get().GetContext().Vfs();
			if (vfs)
			{
				std::error_code ec;
				std::vector<uint8_t> bytes;
				if (vfs->Read(scriptFilePath, bytes, ec) && !bytes.empty())
					return bytes;
			}
			const std::filesystem::path diskPath =
				WLD_ASSETPATH + std::string("/") + scriptFilePath;
			std::ifstream file(diskPath, std::ios::binary);
			if (file.is_open())
			{
				std::stringstream buffer;
				buffer << file.rdbuf();
				const std::string text = buffer.str();
				return std::vector<uint8_t>(text.begin(), text.end());
			}
			throw std::logic_error("Script not found: " + scriptFilePath);
		}

		// 既有签名/错误文本保留:仅供"确定是源码文本"的路径使用(容器字节不经过它)。
		std::string ReadScriptSource(const std::string& scriptFilePath)
		{
			const std::vector<uint8_t> bytes = ReadScriptBytes(scriptFilePath);
			return std::string(bytes.begin(), bytes.end());
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

		// W7-3:容器字节(不嵌源码)跳过 `---@field` 注解解析 → 字段类型走"旧值推断"回退;
		// 源码字节保持既有注解解析。判定只看前 4 字节 magic(与 LoadChunk 同一条判定)。
		std::unordered_map<std::string, std::string> ParseAnnotationsForBytes(const std::vector<uint8_t>& bytes)
		{
			if (Asset::ScriptArtifact::IsArtifactBytes(bytes.data(), bytes.size()))
				return {};
			return ParseFieldAnnotationsInternal(std::string(bytes.begin(), bytes.end()));
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

		// W5a-2:从"当前脚本表"读一个字段的运行期值,并转成新版本声明的字段类型。
		// 迁移优先级:活表(同名且类型兼容)→ CachedFields(同名同类型)→ 新脚本默认值。
		// 只有 rawget 非 nil(真正的自有字段)才算"当前表里有";类型兼容只放宽 number 域:
		//   声明 Int   → 活值必须是整数值且在 int 范围内;
		//   声明 Float → 活值可以是任何 number(整数也可以);
		//   Bool/String → 严格同类型。
		bool TryReadLiveField(const ScriptTableRef& table, const std::string& name, LuaFieldType type,
			LuaScriptField* out)
		{
			if (!out || !table.IsValid() || !table.HasField(name.c_str()))
				return false;

			const ScriptValue value = table.GetField(name.c_str());
			LuaScriptField field;
			field.Type = type;
			switch (type)
			{
				case LuaFieldType::Int:
				{
					double number = 0.0;
					if (!value.AsNumber(&number) || !std::isfinite(number) || number != std::floor(number) ||
						number < (std::numeric_limits<int>::min)() || number > (std::numeric_limits<int>::max)())
						return false;
					field.Value = static_cast<int>(number);
					break;
				}
				case LuaFieldType::Float:
				{
					double number = 0.0;
					if (!value.AsNumber(&number))
						return false;
					field.Value = static_cast<float>(number);
					break;
				}
				case LuaFieldType::Bool:
				{
					bool boolean = false;
					if (!value.AsBool(&boolean))
						return false;
					field.Value = boolean;
					break;
				}
				case LuaFieldType::String:
				{
					std::string text;
					if (!value.AsString(&text))
						return false;
					field.Value = std::move(text);
					break;
				}
				default:
					return false;
			}
			*out = std::move(field);
			return true;
		}

		// 脚本返回表 -> 内存字段缓存(键仍是字段名;同类型才复用旧值 —— 与 sol2 时期一致)。
		// liveTable(可空)只用于热重载:活表同名兼容值优先于 CachedFields。
		std::unordered_map<std::string, LuaScriptField> BuildFieldCache(const ScriptTableRef& table,
			const std::unordered_map<std::string, std::string>& schema, const LuaScriptComponent& script,
			const ScriptTableRef* liveTable = nullptr)
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
				// W5a-2 迁移源优先级:活表(运行期 self.X=... 的真实状态)→ CachedFields → 新默认值。
				bool migrated = false;
				if (liveTable)
				{
					LuaScriptField live;
					if (TryReadLiveField(*liveTable, name, field.Type, &live))
					{
						field.Value = std::move(live.Value);
						migrated = true;
					}
				}
				if (!migrated)
				{
					const auto old = script.CachedFields.find(name);
					if (old != script.CachedFields.end() && old->second.Type == field.Type)
						field.Value = old->second.Value;
				}
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

		// W4:四个生命周期回调(OnCreate/OnUpdate/OnDestroy/OnUI)周围的"当前脚本实例"作用域。
		// events:on / timers:after/every 借它把订阅归属到 (场景令牌, 实体句柄含版本, 组件 id, Generation)。
		ScriptEventOwner MakeScriptEventOwner(const Entity& entity, uint64_t generation)
		{
			ScriptEventOwner owner;
			owner.EntityRef = entity;
			owner.ScenePtr = entity.IsValid() ? entity.GetScene() : nullptr;
			owner.Component = entt::type_id<LuaScriptComponent>().hash();
			owner.Generation = generation;
			return owner;
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

		// W7-3:编译并在**独立 environment** 里执行脚本(容器/源码统一走 LuauVm::LoadChunk),
		// 取出返回的表。容器头命中但校验失败时 LoadChunk 硬失败 → 这里抛它的原始错误文本
		// (绝不回退按源码编译)。
		// chunkName 用逻辑脚本路径:错误文本里的文件/行号要能被编辑器直接定位。
		ScriptTableRef InstantiateScriptTable(const std::vector<uint8_t>& bytes, const char* chunkName,
			const ScriptTableRef& environment)
		{
			ScriptTableRef table;
			std::string error;
			ScriptFunctionRef chunk = s_Vm->LoadChunk(bytes, chunkName, environment, &error);
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
			// W4:VM 关闭前先丢弃事件/计时器订阅(引用在 VM 关闭后一律失效)。
			ResetScriptEventSubscriptions();
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
			// W6:默认预算在 VM 建立后立即下发(所有绑定/脚本调用都在它之下)。
			Sandbox::SetDefaultPolicy(s_Vm->State(), s_SandboxPolicy);
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
			// W3a-A1:组件字段代理类型(Entity:GetComponent 的返回值;映射表见 Script/BindComponentAccess.h)。
			if (!RegisterComponentProxyBinding(*s_Bindings, &error))
				throw std::runtime_error("[Lua] failed to register the component proxy binding: " + error);
			// W3b:游戏服务面(Input/Level/Save 三个只读全局表;参数与失败语义见 Script/BindServices.h)。
			if (!RegisterGameplayServiceBindings(*s_Bindings, &error))
				throw std::runtime_error("[Lua] failed to register the gameplay service bindings: " + error);
			// W3c:UI 面(只读全局表 `ui`;宿主入口 DrawScriptUi 见 Script/BindUI.h)。
			if (!RegisterUiBindings(*s_Bindings, &error))
				throw std::runtime_error("[Lua] failed to register the UI bindings: " + error);
			// W4:事件/计时器面(只读全局表 `events`/`timers`;见 Script/BindEvents.h)。
			if (!RegisterEventBindings(*s_Bindings, &error))
				throw std::runtime_error("[Lua] failed to register the event/timer bindings: " + error);
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
		if (s_Vm)
		{
			AssertOwnerThread();
			// Hosts release all scene/preview references before entering this function.
			ShutdownInternal();
		}
		// W7-3:登记不要求宿主反登记,但引擎关闭后不能再持有宿主的内容上下文
		// (宿主可能随后销毁 WorldContext;下一次 Init() 后如需包内容请重新登记)。
		s_ContentContext = nullptr;
	}

	void ScriptEngine::Init(WorldContext& context)
	{
		// W7-3:只登记内容上下文,不触碰 VM 生命周期(可在 Init() 之前或之后调用;可重复覆盖)。
		s_ContentContext = &context;
	}

	// P2 W8:逻辑脚本路径 → 可编辑的磁盘绝对路径(声明见 Script/HotReload.h)。
	// 实现落在本文件是为了复用 ReadScriptBytes 的 VFS 选择顺序:登记的内容上下文优先,
	// 未登记回退 Application(s_ContentContext 是本文件的文件内静态)。
	// 语义:Vfs::Normalize 校验(拒绝绝对路径/盘符/"."/".." 段)→ VFS 命中且来源是 Package
	// → 报"包内不可编辑";否则要求 WLD_ASSETPATH/<path> 是常规文件,成功时返回其绝对路径。
	bool ResolveScriptDiskPath(std::string_view logicalPath, std::filesystem::path& out, std::string* error)
	{
		Vfs::Path normalized;
		std::error_code normalizeError;
		if (!Vfs::Normalize(logicalPath, normalized, normalizeError))
		{
			if (error)
				*error = "invalid script path (absolute paths and `.`/`..` segments are rejected): "
					+ std::string(logicalPath);
			return false;
		}

		const Vfs::Vfs* vfs = nullptr;
		if (s_ContentContext)
			vfs = &s_ContentContext->Vfs();          // U3:headless 只挂包 provider
		else if (Application::HasInstance())
			vfs = &Application::Get().GetContext().Vfs();
		if (vfs)
		{
			// 包内脚本是编译产物:没有可编辑的源文件,只有磁盘(开发树)上的脚本可写/可打开。
			Vfs::StatInfo stat;
			std::error_code statError;
			if (vfs->Resolve(normalized, stat, statError) && stat.source == Vfs::Source::Package)
			{
				if (error)
					*error = "包内不可编辑(脚本来自包): " + normalized;
				return false;
			}
		}

		const std::filesystem::path diskPath =
			std::filesystem::path(WLD_ASSETPATH) / std::filesystem::path(normalized);
		std::error_code fileError;
		if (!std::filesystem::is_regular_file(diskPath, fileError))
		{
			if (error)
				*error = "script file not found on disk: " + diskPath.string();
			return false;
		}

		std::error_code absoluteError;
		const std::filesystem::path absolute = std::filesystem::absolute(diskPath, absoluteError);
		out = absoluteError ? diskPath : absolute;
		if (error)
			error->clear();
		return true;
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

	void ScriptEngine::SetSandboxPolicy(const Sandbox::Policy& policy)
	{
		if (IsInitialized()) AssertOwnerThread();
		s_SandboxPolicy = policy;
		if (s_Vm) Sandbox::SetDefaultPolicy(s_Vm->State(), policy);
	}

	Sandbox::Policy ScriptEngine::GetSandboxPolicy()
	{
		if (IsInitialized()) AssertOwnerThread();
		return s_Vm ? Sandbox::GetDefaultPolicy(s_Vm->State()) : s_SandboxPolicy;
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
		// W3a-A2:宿主存在时把 schema 注册表带进生成链路,组件块随 WE_FIELD 自动更新;
		// 无 Application 的纯工具/测试进程没有注册表,保持既有"仅 Lua 类型"输出。
		if (Application::HasInstance())
			return GenerateLuaStubs(Application::Get().GetContext().Schemas());

		std::string error;
		const std::filesystem::path path(WLD_ASSETPATH + std::string("/scripts/intermediate/WorldEngineAPI.luau"));
		if (!LuaStubGenerator::Generate(path, error))
		{
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("[Lua] {0}", error);
			return false;
		}
		return true;
	}

	bool ScriptEngine::GenerateLuaStubs(const Schema::SchemaRegistry& schemas)
	{
		AssertOwnerThread();
		std::string error;
		const std::filesystem::path path(WLD_ASSETPATH + std::string("/scripts/intermediate/WorldEngineAPI.luau"));
		const std::vector<const Schema::TypeSchema*> components = schemas.List(Schema::TypeCategory::Component);
		std::size_t serviceCount = 0;
		const ScriptServiceBinding* services = GameplayServiceBindings(&serviceCount);
		std::vector<const ScriptServiceBinding*> serviceList;
		serviceList.reserve(serviceCount);
		for (std::size_t index = 0; index < serviceCount; ++index)
			serviceList.push_back(&services[index]);
		// W3c:UI 块渲染在服务块之后、组件块之前(与脚本可见的全局表顺序一致)。
		std::size_t uiCount = 0;
		const ScriptServiceBinding* uiTables = ScriptUiBindings(&uiCount);
		std::vector<const ScriptServiceBinding*> uiList;
		uiList.reserve(uiCount);
		for (std::size_t index = 0; index < uiCount; ++index)
			uiList.push_back(&uiTables[index]);
		// W4:events/timers 与 Input/Level/Save 同属"全局只读表",沿用服务块的渲染链路
		// (描述表在 BindEvents.cpp,不改 LuaStubGenerator)。
		std::size_t eventCount = 0;
		const ScriptServiceBinding* eventTables = ScriptEventBindings(&eventCount);
		for (std::size_t index = 0; index < eventCount; ++index)
			serviceList.push_back(&eventTables[index]);
		if (!LuaStubGenerator::Generate(path, LuaReflectionRegistry::GetTable(), components, serviceList, uiList, error))
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
			// 1. 读取脚本字节;源码才静态解析 ---@field 注解(容器不嵌源码,跳过注解)。
			const std::vector<uint8_t> bytes = ReadScriptBytes(script.ScriptFilePath);
			std::unordered_map<std::string, std::string> schema = ParseAnnotationsForBytes(bytes);

			// 2. 在独立 environment 里执行脚本，获取默认值表（不保留引用：编辑器预览只缓存字段）。
			ScriptTableRef environment = s_Vm->CreateEnvironment();
			if (!environment.IsValid()) throw std::logic_error("Cannot create a script environment");
			ScriptTableRef table = InstantiateScriptTable(bytes, script.ScriptFilePath.c_str(), environment);

			// 3. 构建字段：类型优先取注解，缺失注解走旧值推断兼容路径。
			script.CachedFields = BuildFieldCache(table, schema, script);
			// W2a：把该脚本登记成 Luau 行为描述（字段来自 CachedFields）。只写行为注册表，
			// 不改变加载/预览语义：登记失败只记日志，返回值与状态机仍由下面的旧逻辑决定。
			std::string behaviorError;
			if (!EnsureLuaBehavior(script, &behaviorError) && Log::GetCoreLogger())
				WLD_CORE_WARN("[Behavior] {0}", behaviorError);
			script.LastError.clear();
			script.State = ScriptInstanceState::Stopped;
			// W5a-2/W7-3:加载成功即建立源指纹基线(容器 = 容器字节,源码 = 源码字节),
			// 并清掉上一次遗留的重载诊断。基线必须与本次装载用的是同一份字节。
			script.SourceFingerprint = FingerprintScriptBytes(bytes.data(), bytes.size());
			script.ReloadDiagnostic.clear();
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
			// W7-3:读取原始字节;容器 → 跳过注解解析,源码 → 既有注解解析。
			const std::vector<uint8_t> bytes = ReadScriptBytes(script.ScriptFilePath);
			ScriptTableRef environment = s_Vm->CreateEnvironment();
			if (!environment.IsValid()) throw std::logic_error("Cannot create a script environment");
			ScriptTableRef table = InstantiateScriptTable(bytes, script.ScriptFilePath.c_str(), environment);
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
			// W3c:UI 阶段回调;缺失与其它三个一致(nil = 不调用,不是错误)。
			if (!ReadCallback(script.ScriptTable, "OnUI", &script.OnUiFunc, &error))
				throw std::runtime_error(error);

			script.IsLoaded = true;
			script.CreateEntered = true;
			phase = "OnCreate";
			if (script.OnCreateFunc.IsValid())
			{
				// W4:回调期间 events:on / timers:after/every 归属本实例。
				const ScriptEventOwnerScope ownerScope(MakeScriptEventOwner(script.RuntimeEntity, script.Generation));
				const ScriptValue args[] = { script.ScriptTable.ToValue() };
				if (!script.OnCreateFunc.Call(args, 1, nullptr, &error))
					throw std::runtime_error(error);
			}
			script.State = ScriptInstanceState::Running;
			// W5a-2/W7-3:运行期首次加载成功同样建立指纹基线(与本次装载同一份字节;
			// 监听器不改文件时不产生假阳性)。
			script.SourceFingerprint = FingerprintScriptBytes(bytes.data(), bytes.size());
			script.ReloadDiagnostic.clear();
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
				// W4:回调期间 events:on / timers:after/every 归属本实例。
				const ScriptEventOwnerScope ownerScope(MakeScriptEventOwner(script.RuntimeEntity, script.Generation));
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
			script.OnCreateFunc.IsValid() || script.OnUpdateFunc.IsValid() || script.OnDestroyFunc.IsValid() ||
			script.OnUiFunc.IsValid())
			throw std::logic_error("Script references must be released before ScriptEngine::Shutdown");
		if (script.State == ScriptInstanceState::Destroying) return;
		bool faulted = script.State == ScriptInstanceState::Faulted;
		script.State = ScriptInstanceState::Destroying;
		// W4:实例销毁时批量退订它的事件/计时器订阅(OnDestroy 里新建的订阅也一并丢弃)。
		const Entity ownerEntity = script.RuntimeEntity;
		const uint64_t ownerGeneration = script.Generation;
		try
		{
			const bool entered = script.CreateEntered;
			script.CreateEntered = false;
			if (entered && script.OnDestroyFunc.IsValid())
			{
				const ScriptEventOwnerScope ownerScope(MakeScriptEventOwner(ownerEntity, ownerGeneration));
				std::string error;
				const ScriptValue args[] = { script.ScriptTable.ToValue() };
				if (!script.OnDestroyFunc.Call(args, 1, nullptr, &error))
					throw std::runtime_error(error);
			}
		}
		catch (const std::exception& error) { ReportLuaError(script, "OnDestroy", error.what()); faulted = true; }
		catch (...) { ReportLuaError(script, "OnDestroy", "Unknown exception"); faulted = true; }
		ReleaseScriptEventOwners(ownerEntity, ownerGeneration);
		ClearLuaReferences(script);
		script.State = faulted ? ScriptInstanceState::Faulted : ScriptInstanceState::Stopped;
	}

	std::size_t ScriptEngine::DrawScriptUi(Scene& scene, Wui::WuiContext& context)
	{
		AssertOwnerThread();
		std::size_t failures = 0;
		// 只读枚举必须走 const 路径:Running/活动场景上的非 const GetRegistry() 会触发结构写断言。
		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		const auto view = registry.view<LuaScriptComponent>();
		for (const entt::entity handle : view)
		{
			// 引用本身来自 const view,但这里只写组件的运行态字段(State/LastError),
			// 不会增删实体或组件,因此不会触发结构写断言。
			LuaScriptComponent& script =
				const_cast<LuaScriptComponent&>(view.get<LuaScriptComponent>(handle));
			if (!script.IsLoaded || script.State != ScriptInstanceState::Running ||
				!script.ScriptTable.IsValid() || !script.OnUiFunc.IsValid())
				continue;
			try
			{
				ScriptUiScope scope(context, script.ScriptFilePath);
				// W4:OnUI 期间 events:on / timers:after/every 同样归属本实例。
				const ScriptEventOwnerScope ownerScope(
					MakeScriptEventOwner(Entity(&scene, handle), script.Generation));
				std::string error;
				const ScriptValue args[] = { script.ScriptTable.ToValue() };
				if (!script.OnUiFunc.Call(args, 1, nullptr, &error))
					throw std::runtime_error(error);
			}
			catch (const std::exception& error) { ReportLuaError(script, "OnUI", error.what()); ++failures; }
			catch (...) { ReportLuaError(script, "OnUI", "Unknown exception"); ++failures; }
		}
		return failures;
	}

	// ---- P2 W2a:行为注册层（只登记/查询，不参与调度）----

	BehaviorRegistry& ScriptEngine::Behaviors()
	{
		return BehaviorRegistry::Instance();
	}

	bool ScriptEngine::EnsureLuaBehavior(LuaScriptComponent& script, std::string* error)
	{
		if (script.ScriptFilePath.empty())
		{
			if (error) *error = "lua behavior requires a non-empty script path";
			return false;
		}
		BehaviorRegistry& registry = BehaviorRegistry::Instance();
		BehaviorDesc desc = BehaviorRegistry::MakeLuaDesc(script);
		const BehaviorDesc* existing = registry.Find(desc.ModuleId);
		if (!existing) return registry.Register(std::move(desc), error);
		if (BehaviorDescEquals(*existing, desc)) return true; // 幂等：同模块、同描述
		return registry.Replace(std::move(desc), error);      // 脚本字段变化（编辑/热重载）→ 刷新
	}

	std::size_t ScriptEngine::EnsureSchemaBehaviors(const Schema::SchemaRegistry& schemas, std::vector<std::string>* errors)
	{
		BehaviorRegistry& registry = BehaviorRegistry::Instance();
		std::size_t ensured = 0;
		for (const Schema::TypeSchema* type : schemas.List(Schema::TypeCategory::Script))
		{
			std::string error;
			BehaviorDesc desc = BehaviorRegistry::MakeNativeDesc(*type);
			const BehaviorDesc* existing = registry.Find(desc.ModuleId);
			bool ok = false;
			if (!existing) ok = registry.Register(std::move(desc), &error);
			else if (BehaviorDescEquals(*existing, desc)) ok = true;
			else ok = registry.Replace(std::move(desc), &error);
			if (ok) ++ensured;
			else if (errors) errors->push_back(std::move(error));
		}
		return ensured;
	}

	// ---- P2 W5:L2 脚本热重载(引擎侧)----

	namespace
	{
		// 热重载后的 generation 必须与 Scene 启动期分配的 generation 不同,否则排队中的旧命令
		// 会被 Scene::IsSourceAlive 误判为"仍然有效"(判活条件是 组件 + Generation 相等 + Running)。
		// Scene 的计数器从 1 开始、每次脚本启动 +1(Scene::StartPendingScripts),所以这里把最高位当作
		// "ScriptEngine 热重载域"标记 + 进程内单调序号:
		//   - 与任何 Scene 分配值都不同(Scene 要启动 2^63 次才会撞上);
		//   - 同一组件连续两次重载的 generation 也不同(每次 +1)。
		uint64_t NextReloadGeneration()
		{
			static uint64_t s_ReloadGeneration = 0;
			++s_ReloadGeneration;
			return (uint64_t(1) << 63) | s_ReloadGeneration;
		}

		std::string JoinDiagnosticLines(const std::vector<std::string>& lines)
		{
			std::string text;
			for (const std::string& line : lines)
			{
				if (!text.empty()) text += "\n";
				text += line;
			}
			return text;
		}
	}

	bool ScriptEngine::ReloadScript(LuaScriptComponent& script, std::string* diagnostics)
	{
		AssertOwnerThread();

		const auto reject = [&](const char* phase, const std::string& error)
		{
			script.ReloadDiagnostic = FormatScriptReloadFailure(script.ScriptFilePath, phase, error);
			if (diagnostics) *diagnostics = script.ReloadDiagnostic;
			return false;
		};

		if (script.ScriptFilePath.empty())
			return reject("path check", "script path is empty");

		if (script.State == ScriptInstanceState::Creating)
			return reject("state check", "reload is refused while the instance state is Creating");
		if (script.State == ScriptInstanceState::Destroying)
			return reject("state check", "reload is refused while the instance state is Destroying");

		// 热重载的前提是"有可回滚的旧版本":必须已经加载并且正在运行。
		if (script.State != ScriptInstanceState::Running || !script.IsLoaded ||
			!script.ScriptTable.IsValid() || !script.LuaEnv.IsValid())
			return reject("state check", "reload requires a running script instance; there is no old version to keep");

		// 安全点双保险:宿主应先用 Scene::CanApplyScriptReload() 判定;这里在能取到场景时再校验一次,
		// 避免回调内/结构提交点内/停止流程中把实例引用换掉。
		if (script.RuntimeEntity)
		{
			Scene* scene = script.RuntimeEntity.GetScene();
			if (scene && !scene->CanApplyScriptReload())
				return reject("safe point",
					"the owning scene is inside a script callback, a structural commit, or the stop flow");
		}

		// W7-3:重载读的也是原始字节(容器 → 字节码;源码 → 文本),指纹与本次装载同一份字节。
		std::vector<uint8_t> bytes;
		try { bytes = ReadScriptBytes(script.ScriptFilePath); }
		catch (const std::exception& error) { return reject("read", error.what()); }
		catch (...) { return reject("read", "unknown exception while reading the script bytes"); }
		const uint64_t fingerprint = FingerprintScriptBytes(bytes.data(), bytes.size());

		// 新版本的所有产物先落在局部变量里;任何一步失败都不触碰组件现有引用(失败保留旧版本)。
		ScriptTableRef newEnvironment;
		ScriptTableRef newTable;
		{
			try { newEnvironment = s_Vm->CreateEnvironment(); }
			catch (const std::exception& error) { return reject("environment", error.what()); }
			catch (...) { return reject("environment", "unknown exception while creating a script environment"); }
			if (!newEnvironment.IsValid())
				return reject("environment", "cannot create a script environment");
			try { newTable = InstantiateScriptTable(bytes, script.ScriptFilePath.c_str(), newEnvironment); }
			catch (const std::exception& error) { return reject("load", error.what()); }
			catch (...) { return reject("load", "unknown exception while compiling the new script version"); }
		}

		ScriptFunctionRef newCreate;
		ScriptFunctionRef newUpdate;
		ScriptFunctionRef newDestroy;
		ScriptFunctionRef newUi;
		try
		{
			std::string callbackError;
			if (!ReadCallback(newTable, "OnCreate", &newCreate, &callbackError) ||
				!ReadCallback(newTable, "OnUpdate", &newUpdate, &callbackError) ||
				!ReadCallback(newTable, "OnDestroy", &newDestroy, &callbackError) ||
				!ReadCallback(newTable, "OnUI", &newUi, &callbackError))
				return reject("callbacks", callbackError);
		}
		catch (const std::exception& error) { return reject("callbacks", error.what()); }
		catch (...) { return reject("callbacks", "unknown exception while reading the lifecycle callbacks"); }

		std::unordered_map<std::string, LuaScriptField> newFields;
		std::vector<std::string> warnings;
		try
		{
			// entity 句柄:与 OnCreateScript 同一条路径(三个名字都是同一个非 owning 句柄)。
			const ScriptValue entityValue = MakeEntityValue(*s_Bindings, script.RuntimeEntity);
			if (!newTable.SetField("entity", entityValue) ||
				!newTable.SetField("__Entity", entityValue) ||
				!newTable.SetField("__EntityID", entityValue))
				return reject("bind", "cannot assign the entity handle");

			// 字段迁移:当前活表优先(运行期 self.X=... 只存在于这里),缺失/类型不符再回退 CachedFields,
			// 两者都没有才用新脚本默认值。
			newFields = BuildFieldCache(newTable, ParseAnnotationsForBytes(bytes), script, &script.ScriptTable);
			DescribeScriptFieldMigration(script.CachedFields, newFields, script.ScriptFilePath, &warnings);

			// 把合并后的字段写进**新表**(旧表保持原值,直到整体交换成功)。
			LuaScriptComponent staging;
			staging.ScriptTable = newTable;
			staging.CachedFields = newFields;
			ApplyCachedFields(staging);
		}
		catch (const std::exception& error) { return reject("migration", error.what()); }
		catch (...) { return reject("migration", "unknown exception while migrating the script fields"); }

		// 行为描述刷新:先按"新字段 + 同一路径"替换;失败说明描述非法,旧版本(含旧描述)原样保留。
		{
			LuaScriptComponent staging;
			staging.ScriptFilePath = script.ScriptFilePath;
			staging.CachedFields = newFields;
			std::string behaviorError;
			if (!BehaviorRegistry::Instance().Replace(BehaviorRegistry::MakeLuaDesc(staging), &behaviorError))
				return reject("behavior", behaviorError);
		}

		// 整体交换:引用(环境/脚本表/四个回调)+ 字段 + 指纹 + generation。
		// State / IsLoaded / CreateEntered / LastError 语义不动(失败路径也从未碰过它们)。
		const uint64_t previousGeneration = script.Generation;
		script.LuaEnv = newEnvironment;
		script.ScriptTable = newTable;
		script.OnCreateFunc = newCreate;
		script.OnUpdateFunc = newUpdate;
		script.OnDestroyFunc = newDestroy;
		script.OnUiFunc = newUi;
		script.CachedFields = newFields;
		script.SourceFingerprint = fingerprint;
		script.Generation = NextReloadGeneration();
		script.ReloadDiagnostic.clear();
		// W4:热重载成功 = 旧 instance 的事件/计时器订阅整体作废(旧闭包绝不能再被调用);
		// 新订阅由新版本的 OnCreate 在下一次实例创建/复活(Pending 路径)时建立。
		// 失败路径在上面已经 return,旧订阅与旧回调原样保留。
		ReleaseScriptEventOwners(script.RuntimeEntity, previousGeneration);
		if (diagnostics) *diagnostics = JoinDiagnosticLines(warnings);
		return true;
	}

	// ---- P2 W4:事件/计时器回调的失败落点 ----

	void ScriptEngine::FaultScriptInstance(Entity entity, uint64_t generation, const char* phase,
		const std::string& error)
	{
		if (!IsInitialized())
			return;
		AssertOwnerThread();
		if (!entity.IsValid())
			return;
		Scene* scene = entity.GetScene();
		if (!scene)
			return;
		// 只写组件的运行态字段(State/LastError),不增删实体/组件:与 DrawScriptUi 相同,
		// 走 const registry 取引用再 const_cast,避免在活动场景上触发结构写断言。
		const entt::registry& registry = static_cast<const Scene&>(*scene).GetRegistry();
		const LuaScriptComponent* script =
			registry.try_get<LuaScriptComponent>(static_cast<entt::entity>(entity));
		if (!script || script->Generation != generation)
			return;
		ReportLuaError(const_cast<LuaScriptComponent&>(*script), phase, error);
	}
}
