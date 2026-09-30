#include "wldpch.h"
#include "World/Script/PluginScriptLibrary.h"

#include "World/Core/Log.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/BindServices.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		using Plugins::WeScriptCallApi;
		using Plugins::WeScriptFunctionCallback;
		using Plugins::WeScriptFunctionDesc;
		using Plugins::WE_PLUGIN_ABI_VERSION;

		// 签名里的参数个数上限(防御性;存根渲染与方法表都是宿主侧小数组)。
		constexpr std::size_t kMaxScriptFunctionParams = 16;

		bool FailWith(std::string* error, const std::string& message)
		{
			if (error) *error = message;
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
			if (name.empty() || !IsNameStart(name.front())) return false;
			for (char c : name)
				if (!IsNameChar(c)) return false;
			return keywords.count(name) == 0;
		}

		// 单行说明(与 LuaStubGenerator 的 IsDescription 同口径:不能注入注解/控制字符)。
		bool IsDocLine(const std::string& doc)
		{
			if (doc.empty()) return true;
			const std::size_t first = doc.find_first_not_of(' ');
			if (first != std::string::npos && doc[first] == '@') return false;
			return std::none_of(doc.begin(), doc.end(),
				[](unsigned char c) { return c < 32 || c == 127; });
		}

		bool IsAllowedParamType(const std::string& type)
		{
			return type == "number" || type == "integer" || type == "boolean"
				|| type == "string" || type == "any";
		}

		bool IsAllowedReturnType(const std::string& type)
		{
			return type == "number" || type == "integer" || type == "boolean"
				|| type == "string" || type == "nil" || type == "any";
		}

		void SkipSpaces(const std::string& text, std::size_t* position)
		{
			while (*position < text.size() && (text[*position] == ' ' || text[*position] == '\t'))
				++*position;
		}

		// 从 position 读一个标识符(读到 . , : ) ? 空格 或串尾)。
		bool ReadIdentifier(const std::string& text, std::size_t* position, std::string* out)
		{
			const std::size_t start = *position;
			while (*position < text.size() && IsNameChar(text[*position]))
				++*position;
			if (*position == start) return false;
			*out = text.substr(start, *position - start);
			return true;
		}

		// 引擎/沙箱保留的全局名字(与 VM 实例无关的部分;可缓存)。
		const std::set<std::string>& StaticReservedNamespaces()
		{
			static const std::set<std::string> reserved = [] {
				std::set<std::string> names;
				// 引擎自己的只读全局表(BindServices / BindUI / BindEvents)。
				names.insert("Input");
				names.insert("Level");
				names.insert("Save");
				names.insert("ui");
				names.insert("events");
				names.insert("timers");
				names.insert("WorldScript");
				// Luau 标准库名 + 沙箱必须为 nil 的全局(绑定到这些名字上是冲突/误导)。
				std::size_t count = 0;
				const char* const* libraries = LuauVm::AllowedLibraries(&count);
				for (std::size_t index = 0; index < count; ++index)
					if (libraries[index] && libraries[index][0]) names.insert(libraries[index]);
				const char* const* forbidden = LuauVm::ForbiddenGlobals(&count);
				for (std::size_t index = 0; index < count; ++index)
					if (forbidden[index] && forbidden[index][0]) names.insert(forbidden[index]);
				return names;
			}();
			return reserved;
		}

		// 引擎/沙箱保留的全局名字:插件命名空间不能与它们冲突(注册期干净拒绝)。
		// 脚本绑定类型必须**每次现查**(注册表在 ScriptEngine::Init 里才被填充;
		// 绑定期还会用 VM 的真实全局表再校验一次 —— 那是权威判据)。
		bool IsReservedNamespace(const std::string& name)
		{
			if (StaticReservedNamespaces().count(name)) return true;
			for (const LuaTypeReflection& type : LuaReflectionRegistry::GetTable())
				if (type.ClassName == name) return true;
			return false;
		}

		// ---- 账本条目 ----------------------------------------------------------------

		struct Entry
		{
			std::string PluginId;
			std::string Name;       // 完整点分名
			std::string Namespace;  // 全局表名
			std::string Member;     // 表里的函数名
			std::string Signature;  // 注册原文(诊断/面板用)
			std::string Doc;
			std::string ReturnType; // "" = 无返回值声明
			std::vector<std::string> ParamNames;  // 存储 ScriptServiceParam::Name 指针
			std::vector<std::string> ParamTypes;  // 存储 ScriptServiceParam::LuaType 指针
			std::vector<ScriptServiceParam> Params;
			ScriptNativeFunction Function;        // 宿主侧适配器(参数装箱 → WeScriptCallApi)
			ScriptServiceMethod Method;           // 存根渲染用的描述(与运行时同一份)
		};

		struct State
		{
			// map:节点地址稳定(插入/删除都不会移动其它条目;Method.Params 指向条目内部存储)。
			std::map<std::string, Entry> Entries;   // 键 = 完整点分名(全局唯一)
			// 存根渲染快照(RebuildStubTables 重建;指针只在下一次重建之前有效)。
			std::vector<std::string> StubNames;
			std::vector<std::vector<ScriptServiceMethod>> StubMethods;
			std::vector<ScriptServiceBinding> StubBindings;
			std::vector<const ScriptServiceBinding*> StubTablePointers;
			bool Bound = false;                       // 账本已绑定到当前 ScriptEngine VM
			std::set<std::string> CreatedNamespaces;  // 本次绑定会话里由本账本创建的全局表
		};

		State& Registry()
		{
			static State state;
			return state;
		}

		std::vector<const Entry*> SortedEntries()
		{
			std::vector<const Entry*> entries;
			entries.reserve(Registry().Entries.size());
			for (const auto& item : Registry().Entries)
				entries.push_back(&item.second);
			std::sort(entries.begin(), entries.end(), [](const Entry* left, const Entry* right)
			{
				if (left->PluginId != right->PluginId) return left->PluginId < right->PluginId;
				return left->Name < right->Name;
			});
			return entries;
		}

		// 把条目内部的 C 结构指针重新指向**条目自己的**字符串/数组存储。
		// ⚠ 必须在对 Entry 做任何移动(插入容器)之后调用:SSO 短字符串的缓冲区随对象移动
		// 而失效,移动前取的 c_str() 指针会悬垂。
		void PointEntryStorage(Entry& entry)
		{
			for (std::size_t index = 0; index < entry.Params.size(); ++index)
			{
				entry.Params[index].Name = entry.ParamNames[index].c_str();
				entry.Params[index].LuaType = entry.ParamTypes[index].c_str();
			}
			entry.Method.Name = entry.Member.c_str();
			entry.Method.Function = entry.Function;
			entry.Method.Params = entry.Params.empty() ? nullptr : entry.Params.data();
			entry.Method.ParamCount = entry.Params.size();
			entry.Method.ExpectedArgs = entry.Params.size();
			entry.Method.ReturnType = entry.ReturnType.c_str();
			entry.Method.Description = entry.Doc.c_str();
		}

		// 重建存根渲染快照:命名空间升序;组内 = (插件 id, 函数名)升序(SortedEntries)。
		void RebuildStubTables()
		{
			State& state = Registry();
			std::map<std::string, std::vector<const Entry*>> groups;
			for (const Entry* entry : SortedEntries())
				groups[entry->Namespace].push_back(entry);

			state.StubNames.clear();
			state.StubMethods.clear();
			state.StubBindings.assign(groups.size(), ScriptServiceBinding {});
			state.StubTablePointers.clear();
			state.StubTablePointers.reserve(groups.size());

			for (const auto& group : groups)   // std::map 迭代 = 命名空间升序
			{
				state.StubNames.push_back(group.first);
				std::vector<ScriptServiceMethod> methods;
				methods.reserve(group.second.size());
				for (const Entry* entry : group.second)
					methods.push_back(entry->Method);
				state.StubMethods.push_back(std::move(methods));
			}
			// 所有存储定型之后再取指针(StubNames 的 c_str / Methods 的 data 必须指向稳定存储)。
			for (std::size_t bindingIndex = 0; bindingIndex < state.StubBindings.size(); ++bindingIndex)
			{
				ScriptServiceBinding& binding = state.StubBindings[bindingIndex];
				binding.Name = state.StubNames[bindingIndex].c_str();
				binding.Description = "Global plugin script table; functions are registered by plugins "
					"(deterministic order = namespace, then plugin id + function name).";
				binding.Methods = state.StubMethods[bindingIndex].data();
				binding.MethodCount = state.StubMethods[bindingIndex].size();
				state.StubTablePointers.push_back(&binding);
			}
		}

		// ---- 参数/返回值读写(WeScriptCallApi 的桥)------------------------------------

		struct ScriptCallState
		{
			const ScriptValue* Args = nullptr;
			std::size_t Count = 0;
			std::string PluginId;                 // 诊断用(适配器捕获)
			std::string Name;
			std::vector<std::string> ArgStrings;  // 预分配到 Count:GetArgString 的 c_str 稳定
			ScriptValue Result;
			bool Pushed = false;
			bool OverflowLogged = false;
		};

		bool ReadArg(const ScriptCallState& state, uint32_t index, const ScriptValue** out)
		{
			if (!out || index >= state.Count) return false;
			*out = &state.Args[index];
			return true;
		}

		bool GetArgNumberBridge(void* userData, uint32_t index, double* out)
		{
			auto* state = static_cast<ScriptCallState*>(userData);
			const ScriptValue* value = nullptr;
			if (!state || !out || !ReadArg(*state, index, &value)) return false;
			return value->AsNumber(out);
		}

		bool GetArgStringBridge(void* userData, uint32_t index, const char** outUtf8)
		{
			auto* state = static_cast<ScriptCallState*>(userData);
			if (!state || !outUtf8) return false;
			const ScriptValue* value = nullptr;
			if (!ReadArg(*state, index, &value) || !value->IsString()) return false;
			if (index >= state->ArgStrings.size()) return false;
			std::string& storage = state->ArgStrings[index];
			if (!value->AsString(&storage)) return false;
			*outUtf8 = storage.c_str();
			return true;
		}

		bool GetArgBoolBridge(void* userData, uint32_t index, bool* out)
		{
			auto* state = static_cast<ScriptCallState*>(userData);
			const ScriptValue* value = nullptr;
			if (!state || !out || !ReadArg(*state, index, &value)) return false;
			return value->AsBool(out);
		}

		bool PushValue(ScriptCallState& state, const ScriptValue& value)
		{
			if (state.Pushed)
			{
				if (!state.OverflowLogged)
				{
					state.OverflowLogged = true;
					WLD_CORE_WARN("{0}", "[plugin] " + state.PluginId + ": script function '" + state.Name
						+ "' pushed more than one return value; the extra value was ignored");
				}
				return false;
			}
			state.Result = value;
			state.Pushed = true;
			return true;
		}

		bool PushNumberBridge(void* userData, double value)
		{
			auto* state = static_cast<ScriptCallState*>(userData);
			return state && PushValue(*state, ScriptValue::Number(value));
		}

		bool PushStringBridge(void* userData, const char* utf8)
		{
			auto* state = static_cast<ScriptCallState*>(userData);
			return state && PushValue(*state, ScriptValue::String(utf8 ? utf8 : ""));
		}

		bool PushBoolBridge(void* userData, bool value)
		{
			auto* state = static_cast<ScriptCallState*>(userData);
			return state && PushValue(*state, ScriptValue::Boolean(value));
		}

		bool PushNilBridge(void* userData)
		{
			auto* state = static_cast<ScriptCallState*>(userData);
			return state && PushValue(*state, ScriptValue::Nil());
		}

		// 参数装箱 → WeScriptCallApi → 插件回调 → 返回值。
		// 插件抛异常 = 契约违约:转成 Lua error(pcall 可捕),不让异常把 VM 带走。
		ScriptNativeFunction MakeScriptFunctionAdapter(std::string pluginId, std::string name,
			WeScriptFunctionCallback callback, void* userData)
		{
			return [pluginId = std::move(pluginId), name = std::move(name), callback, userData](
				const ScriptValue* args, std::size_t argCount) -> ScriptValue
			{
				ScriptCallState state;
				state.Args = args;
				state.Count = argCount;
				state.PluginId = pluginId;
				state.Name = name;
				state.ArgStrings.resize(argCount);

				WeScriptCallApi call;
				call.UserData = &state;
				call.ArgCount = static_cast<uint32_t>(argCount);
				call.GetArgNumber = &GetArgNumberBridge;
				call.GetArgString = &GetArgStringBridge;
				call.GetArgBool = &GetArgBoolBridge;
				call.PushNumber = &PushNumberBridge;
				call.PushString = &PushStringBridge;
				call.PushBool = &PushBoolBridge;
				call.PushNil = &PushNilBridge;
				try
				{
					callback(userData, &call);
				}
				catch (const std::exception& exception)
				{
					throw std::logic_error("plugin script function '" + name
						+ "' raised an exception: " + exception.what());
				}
				catch (...)
				{
					throw std::logic_error("plugin script function '" + name
						+ "' raised an unknown exception");
				}
				if (!state.Pushed)
					return ScriptValue::Nil();
				return state.Result;
			};
		}

		// ---- 运行时绑定 --------------------------------------------------------------

		bool OnScriptOwnerThread()
		{
			try
			{
				ScriptEngine::AssertOwnerThread();
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool BindEntry(ScriptBindingContext& bindings, const Entry& entry, std::string* error)
		{
			State& state = Registry();
			LuauVm* vm = bindings.Vm();
			if (!vm || !bindings.IsValid())
				return FailWith(error, "the Luau VM is not initialized");

			const ScriptValue existing = vm->GetGlobal(entry.Namespace.c_str());
			ScriptTableRef table;
			bool created = false;
			if (existing.IsNil())
			{
				table = vm->CreateTable();
				if (!table.IsValid())
					return FailWith(error, "failed to create the namespace table '" + entry.Namespace + "'");
				if (!vm->SetGlobal(entry.Namespace.c_str(), table.ToValue()))
					return FailWith(error, "failed to publish the namespace table '" + entry.Namespace + "'");
				created = true;
			}
			else
			{
				if (state.CreatedNamespaces.count(entry.Namespace) == 0)
					return FailWith(error, "global name '" + entry.Namespace + "' is already in use");
				if (!existing.AsTable(&table) || !table.IsValid())
					return FailWith(error, "global name '" + entry.Namespace + "' is not a table");
			}

			const ScriptValue function = bindings.CreateFunction(entry.Name.c_str(), entry.Function);
			if (!function.IsFunction())
			{
				if (created) vm->ClearGlobal(entry.Namespace.c_str());
				return FailWith(error, "failed to create the script function value");
			}
			if (!table.SetField(entry.Member.c_str(), function))
			{
				if (created) vm->ClearGlobal(entry.Namespace.c_str());
				return FailWith(error, "failed to bind the function into table '" + entry.Namespace + "'");
			}
			if (created) state.CreatedNamespaces.insert(entry.Namespace);
			return true;
		}

		void UnbindEntry(const Entry& entry)
		{
			if (!Registry().Bound || !ScriptEngine::IsInitialized() || !OnScriptOwnerThread())
				return;   // VM 已关闭 / 非绑定上下文:账本删除即可,没有 VM 状态要清。
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			LuauVm* vm = bindings.Vm();
			if (!vm) return;
			const ScriptValue existing = vm->GetGlobal(entry.Namespace.c_str());
			ScriptTableRef table;
			if (!existing.AsTable(&table) || !table.IsValid()) return;
			table.SetField(entry.Member.c_str(), ScriptValue::Nil());
			bool remaining = false;
			for (const auto& item : Registry().Entries)
			{
				if (&item.second != &entry && item.second.Namespace == entry.Namespace)
				{
					remaining = true;
					break;
				}
			}
			if (!remaining)
			{
				vm->ClearGlobal(entry.Namespace.c_str());
				Registry().CreatedNamespaces.erase(entry.Namespace);
			}
		}

		// 解析存根签名:"(name: type[?], ...): rettype";空 = 无参数、无返回值声明。
		bool ParseSignature(const char* raw, Entry& entry, std::string* error)
		{
			entry.Signature = raw ? raw : "";
			std::string text = entry.Signature;
			std::size_t position = 0;
			SkipSpaces(text, &position);
			if (position >= text.size())
				return true;   // 空签名:合法(无参数、无返回值声明)

			if (text[position] != '(')
				return FailWith(error, "signature must start with '(' (got '" + text + "')");
			++position;
			SkipSpaces(text, &position);

			struct ParsedParam { std::string Name; std::string Type; bool Required = true; };
			std::vector<ParsedParam> parsed;
			bool sawOptional = false;
			if (position < text.size() && text[position] != ')')
			{
				while (true)
				{
					SkipSpaces(text, &position);
					ParsedParam param;
					if (!ReadIdentifier(text, &position, &param.Name) || !IsIdentifier(param.Name))
						return FailWith(error, "signature parameter " + std::to_string(parsed.size() + 1)
							+ " has an invalid name (expected a Lua identifier)");
					SkipSpaces(text, &position);
					if (position < text.size() && text[position] == '?')
					{
						param.Required = false;
						++position;
						SkipSpaces(text, &position);
					}
					if (position >= text.size() || text[position] != ':')
						return FailWith(error, "signature parameter '" + param.Name + "' is missing ':'");
					++position;
					SkipSpaces(text, &position);
					if (!ReadIdentifier(text, &position, &param.Type) || !IsAllowedParamType(param.Type))
						return FailWith(error, "signature parameter '" + param.Name
							+ "' must be number, integer, boolean, string or any");
					SkipSpaces(text, &position);
					if (position < text.size() && text[position] == '?')
					{
						param.Required = false;
						++position;
						SkipSpaces(text, &position);
					}
					for (const ParsedParam& previous : parsed)
						if (previous.Name == param.Name)
							return FailWith(error, "signature parameter '" + param.Name + "' is declared twice");
					if (!param.Required)
						sawOptional = true;
					else if (sawOptional)
						return FailWith(error, "signature parameter '" + param.Name
							+ "' is required but follows an optional parameter");
					parsed.push_back(std::move(param));
					if (parsed.size() > kMaxScriptFunctionParams)
						return FailWith(error, "signature has more than "
							+ std::to_string(kMaxScriptFunctionParams) + " parameters");
					if (position < text.size() && text[position] == ',')
					{
						++position;
						continue;
					}
					break;
				}
				SkipSpaces(text, &position);
			}
			if (position >= text.size() || text[position] != ')')
				return FailWith(error, "signature is missing the closing ')'");
			++position;
			SkipSpaces(text, &position);
			if (position < text.size() && text[position] == ':')
			{
				++position;
				SkipSpaces(text, &position);
				std::string returnType;
				if (!ReadIdentifier(text, &position, &returnType) || !IsAllowedReturnType(returnType))
					return FailWith(error, "signature return type must be number, integer, boolean, string, nil or any");
				entry.ReturnType = returnType;
				SkipSpaces(text, &position);
			}
			if (position != text.size())
				return FailWith(error, "signature has trailing text after the parameter list");

			// 先定型字符串存储,再填 ScriptServiceParam 指针(指针必须指向稳定存储)。
			entry.ParamNames.reserve(parsed.size());
			entry.ParamTypes.reserve(parsed.size());
			for (const ParsedParam& param : parsed)
			{
				entry.ParamNames.push_back(param.Name);
				entry.ParamTypes.push_back(param.Type);
			}
			entry.Params.resize(parsed.size());
			for (std::size_t index = 0; index < parsed.size(); ++index)
			{
				ScriptServiceParam& target = entry.Params[index];
				target.Name = entry.ParamNames[index].c_str();
				target.LuaType = entry.ParamTypes[index].c_str();
				target.Accepted = ScriptServiceArgType::None;
				target.Required = parsed[index].Required;
				target.Description = "";
			}
			return true;
		}

		Entry* FindEntry(const std::string& name)
		{
			const auto found = Registry().Entries.find(name);
			return found == Registry().Entries.end() ? nullptr : &found->second;
		}
	}

	bool PluginScriptLibrary::Register(const std::string& pluginId,
		const Plugins::WeScriptFunctionDesc& desc, std::string* error)
	{
		if (pluginId.empty())
			return FailWith(error, "empty plugin id");
		if (desc.StructSize < offsetof(Plugins::WeScriptFunctionDesc, Callback) + sizeof(desc.Callback))
			return FailWith(error, "script function descriptor struct too small (StructSize="
				+ std::to_string(desc.StructSize) + ")");
		if (desc.AbiVersion != WE_PLUGIN_ABI_VERSION)
			return FailWith(error, "script function descriptor ABI mismatch (descriptor="
				+ std::to_string(desc.AbiVersion) + " host=" + std::to_string(WE_PLUGIN_ABI_VERSION) + ")");
		if (!desc.Name || !desc.Name[0])
			return FailWith(error, "script function name is empty");
		if (!desc.Callback)
			return FailWith(error, "script function '" + std::string(desc.Name) + "' has no callback");

		Entry entry;
		entry.PluginId = pluginId;
		entry.Name = desc.Name;
		const std::size_t dot = entry.Name.find('.');
		if (dot == std::string::npos || entry.Name.find('.', dot + 1) != std::string::npos)
			return FailWith(error, "script function name '" + entry.Name
				+ "' must be '<namespace>.<function>' (exactly one dot)");
		entry.Namespace = entry.Name.substr(0, dot);
		entry.Member = entry.Name.substr(dot + 1);
		if (!IsIdentifier(entry.Namespace) || !IsIdentifier(entry.Member))
			return FailWith(error, "script function name '" + entry.Name
				+ "' must use valid Lua identifiers for both segments");
		if (IsReservedNamespace(entry.Namespace))
			return FailWith(error, "script function namespace '" + entry.Namespace
				+ "' is reserved by the engine");
		entry.Doc = desc.Doc ? desc.Doc : "";
		if (!IsDocLine(entry.Doc))
			return FailWith(error, "script function '" + entry.Name
				+ "' doc must be a single comment line");
		if (!ParseSignature(desc.Signature, entry, error))
			return false;

		// 全局唯一:跨插件同名字、本插件内重复都拒绝(同一命名空间的不同函数可以共享)。
		const auto duplicate = Registry().Entries.find(entry.Name);
		if (duplicate != Registry().Entries.end())
			return FailWith(error, "script function '" + entry.Name
				+ "' is already registered by plugin '" + duplicate->second.PluginId + "'");

		entry.Function = MakeScriptFunctionAdapter(pluginId, entry.Name, desc.Callback, desc.UserData);
		entry.Method.Name = entry.Member.c_str();
		entry.Method.Function = entry.Function;
		entry.Method.Params = entry.Params.empty() ? nullptr : entry.Params.data();
		entry.Method.ParamCount = entry.Params.size();
		entry.Method.ExpectedArgs = entry.Params.size();
		entry.Method.ReturnType = entry.ReturnType.c_str();
		entry.Method.Description = entry.Doc.c_str();

		// VM 可用 = 立即绑定(失败 = 干净拒绝,不入账本);否则只记账,Init 时统一绑定。
		if (ScriptEngine::IsInitialized())
		{
			if (!OnScriptOwnerThread())
				return FailWith(error, "script function '" + entry.Name
					+ "' registration must run on the script owner thread");
			std::string bindError;
			if (!BindEntry(ScriptEngine::GetBindingContext(), entry, &bindError))
				return FailWith(error, "script function '" + entry.Name + "' could not be bound: " + bindError);
		}

		// 入账本(map 节点稳定);插入之后再指向条目自己的存储(移动会让 SSO 指针失效)。
		// 键单独拷贝一份:emplace 的实参求值顺序不保证"先键后值",直接传 entry.Name 有
		// 被 move 提前掏空的风险。
		const std::string functionName = entry.Name;
		const auto inserted = Registry().Entries.emplace(functionName, std::move(entry));
		PointEntryStorage(inserted.first->second);
		RebuildStubTables();
		if (error) error->clear();
		return true;
	}

	bool PluginScriptLibrary::Unregister(const std::string& pluginId, const std::string& name,
		std::string* error)
	{
		if (pluginId.empty() || name.empty())
			return FailWith(error, "unregister needs a plugin id and a script function name");
		const auto found = Registry().Entries.find(name);
		if (found != Registry().Entries.end())
		{
			if (found->second.PluginId != pluginId)
				return FailWith(error, "script function '" + name + "' is owned by plugin '"
					+ found->second.PluginId + "'; unregister ignored");
			UnbindEntry(found->second);
			Registry().Entries.erase(found);
			RebuildStubTables();
			if (error) error->clear();
			return true;
		}
		if (error) error->clear();
		return true;   // 幂等:没注册过(或已被兜底回收)= true
	}

	std::size_t PluginScriptLibrary::UnregisterAll(const std::string& pluginId)
	{
		if (pluginId.empty())
			return 0;
		std::size_t removed = 0;
		for (auto it = Registry().Entries.begin(); it != Registry().Entries.end();)
		{
			if (it->second.PluginId != pluginId)
			{
				++it;
				continue;
			}
			UnbindEntry(it->second);
			it = Registry().Entries.erase(it);
			++removed;
		}
		if (removed > 0)
			RebuildStubTables();
		return removed;
	}

	std::size_t PluginScriptLibrary::Count()
	{
		return Registry().Entries.size();
	}

	bool PluginScriptLibrary::IsRegistered(const std::string& pluginId, const std::string& name)
	{
		const Entry* entry = FindEntry(name);
		return entry && entry->PluginId == pluginId;
	}

	std::string PluginScriptLibrary::OwnerOf(const std::string& name)
	{
		const Entry* entry = FindEntry(name);
		return entry ? entry->PluginId : std::string();
	}

	bool PluginScriptLibrary::Describe(const std::string& pluginId, const std::string& name,
		PluginScriptFunctionInfo* out)
	{
		const Entry* entry = FindEntry(name);
		if (!entry || entry->PluginId != pluginId)
			return false;
		if (out)
		{
			out->PluginId = entry->PluginId;
			out->Name = entry->Name;
			out->Namespace = entry->Namespace;
			out->Member = entry->Member;
			out->Signature = entry->Signature;
			out->Doc = entry->Doc;
		}
		return true;
	}

	std::vector<PluginScriptFunctionInfo> PluginScriptLibrary::Snapshot()
	{
		std::vector<PluginScriptFunctionInfo> snapshot;
		snapshot.reserve(Registry().Entries.size());
		for (const Entry* entry : SortedEntries())
		{
			PluginScriptFunctionInfo info;
			info.PluginId = entry->PluginId;
			info.Name = entry->Name;
			info.Namespace = entry->Namespace;
			info.Member = entry->Member;
			info.Signature = entry->Signature;
			info.Doc = entry->Doc;
			snapshot.push_back(std::move(info));
		}
		return snapshot;
	}

	const std::vector<const ScriptServiceBinding*>& PluginScriptLibrary::StubTables()
	{
		return Registry().StubTablePointers;
	}

	bool PluginScriptLibrary::BindAll(ScriptBindingContext& bindings, std::string* error)
	{
		if (!bindings.IsValid() || !bindings.Vm())
			return FailWith(error, "the script binding context is not initialized");
		State& state = Registry();
		state.Bound = false;
		state.CreatedNamespaces.clear();
		// 确定性绑定顺序 = (插件 id, 函数名)升序;命名空间由第一个条目创建,后续条目复用。
		for (const Entry* entry : SortedEntries())
		{
			std::string bindError;
			if (!BindEntry(bindings, *entry, &bindError))
			{
				// 真正的拒绝发生在注册期;这里只剩"VM 之前注册 + 与内建全局冲突"的边角
				// 情况 —— 记 ERROR 并跳过该条,不让引擎启动失败。
				WLD_CORE_ERROR("{0}", "[plugin] " + entry->PluginId + ": script function '"
					+ entry->Name + "' could not be bound: " + bindError);
			}
		}
		state.Bound = true;
		if (error) error->clear();
		return true;
	}

	void PluginScriptLibrary::OnVmShutdown()
	{
		Registry().Bound = false;
		Registry().CreatedNamespaces.clear();
	}
}
