#include "wldpch.h"
#include "ScriptEngine.h"
#include "Components.h"
#include "LuaStubGenerator.h"
#include "World/Core/Application.h"
#include "World/Core/Vfs/Vfs.h"
#include "World/Core/Asset/ScriptArtifact.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Script/BindECS.h"
#include "World/Script/BindEvents.h"
#include "World/Script/BindUI.h"
#include "World/Script/BindServices.h"
#include "World/Script/HotReload.h"
#include "World/Script/LuauVm.h"
#include "World/Script/PluginScriptLibrary.h"
#include "World/Script/Sandbox.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptFileWatch.h"
#include "World/Script/ScriptProperties.h"
#include "World/Script/ScriptRef.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiContext.h"

#include <any>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
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
		// Pure ECS: 全局活动场景指针（不拥有；ShutdownInternal 清空）
		Scene* s_ActiveScene = nullptr;

		// Pure ECS: 系统脚本归属表 —— 一个系统脚本文件注册了哪些具名系统。
		// 热重载时按这份表精确撤销该系统脚本注册的系统,再整份重跑。
		struct SystemScriptRecord
		{
			std::string LogicalPath;
			std::vector<std::string> SystemNames;
		};
		std::vector<SystemScriptRecord> s_SystemScripts;
		// 当前正在执行的系统脚本逻辑路径(空 = 不在加载系统脚本)。
		// ecs:AddSystem 用它把系统名归属到发起它的那份系统脚本。
		std::string s_LoadingSystemScript;
		// 系统脚本目录的轮询监听(懒建立;ShutdownInternal 释放)。
		std::unique_ptr<ScriptFileWatch> s_SystemWatch;

		SystemScriptRecord* FindSystemScriptRecord(const std::string& logicalPath)
		{
			for (SystemScriptRecord& record : s_SystemScripts)
				if (record.LogicalPath == logicalPath)
					return &record;
			return nullptr;
		}
		// W6:引擎默认预算(指令 1e6;时间关)。LuauVm 层保持 0 = 不限,
		// 避免改变 World.LuauVm / World.LuauBinding 的既有语义。
		Sandbox::Policy s_SandboxPolicy{ 1000000, 0 };
		// 受保护的 `target[key]` 查找:脚本 __index 抛错时错误要变成可诊断文本,
		// 不能 longjmp 穿过还活着的 C++ 局部对象(与 T1 的表写限制同源)。
		ScriptFunctionRef s_LookupField;
		// 受保护的字段名收集:脚本返回的表可能有 __index 元表,只收集**自有字符串键**。
		ScriptFunctionRef s_CollectFieldNames;
		// C 期(数组/映射):受保护的"自有键条目"收集(string 或 number 键 + 值)与"按键写值"。
		// 数字键在 C++ 侧的脚本表 API(ScriptTableRef::SetField/GetField 是字符串键)上无法表达,
		// 因此这两条走受保护的小 helper(与 s_CollectFieldNames 同一条安全口径)。
		ScriptFunctionRef s_CollectEntries;
		ScriptFunctionRef s_SetIndex;

		// V1:声明查询的单槽缓存(检视器可能每帧调用)。键 = 逻辑路径 + 内容指纹 + VM 是否可用。
		// 只存纯数据(不含 VM 引用),Shutdown/Init 后依然有效;内容变了或 VM 可用性变了自动失效。
		struct DeclarationCache
		{
			bool Valid = false;
			std::string Path;
			uint64_t Fingerprint = 0;
			bool VmAvailable = false;
			std::vector<ScriptProperties::Declaration> Declarations;
			std::vector<std::string> Diagnostics;
		};
		DeclarationCache s_DeclarationCache;
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
		// C 期:收集自有键的条目 {name = tostring(key), number = key, value = value}。
		// wantNumeric = true → 只收 number 键(映射/数组),false → 只收 string 键(结构化表)。
		const char* const kEntryCollectorSource =
			"return function(target, wantNumeric)\n"
			"    local entries = {}\n"
			"    for key, value in pairs(target) do\n"
			"        local kind = type(key)\n"
			"        if (wantNumeric and kind == 'number') or (not wantNumeric and kind == 'string') then\n"
			"            table.insert(entries, { name = tostring(key), number = key, value = value })\n"
			"        end\n"
			"    end\n"
			"    return entries\n"
			"end";
		// C 期:按键写值(数字键的唯一写入口;字符串键也有 SetField,这里只为映射统一)。
		const char* const kSetIndexSource =
			"return function(target, key, value)\n"
			"    target[key] = value\n"
			"end";


		// W7-3:脚本读取的单一入口(二进制安全,容器字节里的 '\0' 原样保留)。
		// 顺序:登记的内容上下文 VFS(未登记时用既有 Application VFS)→ 磁盘
		// <当前内容根>/<逻辑路径>;两者都没命中抛可读错误(文本与既有 ReadScriptSource 一致)。
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
				World::Paths::AssetRoot() / scriptFilePath;
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

		// V1:一条 `---@field` 注解 —— 名字 / Lua 类型名 / 第三段说明(整段剩余文本,去掉首尾空白)。
		// 顺序 = 源码里的注解顺序;重复声明以第一次为准。
		struct FieldAnnotation
		{
			std::string Name;
			std::string TypeName;   // C 期起可能是类型表达式(`{number}` / `{string: number}` / `{{number}}`)
			std::string Doc;
		};
		using AnnotationList = std::vector<FieldAnnotation>;

		std::string TrimWhitespace(const std::string& text)
		{
			const size_t begin = text.find_first_not_of(" \t\r\n");
			if (begin == std::string::npos)
				return {};
			const size_t end = text.find_last_not_of(" \t\r\n");
			return text.substr(begin, end - begin + 1);
		}

		// C 期:把 `---@field` 之后的剩余文本拆成"名字 + 类型表达式 + 说明"。
		// 类型表达式 = 平衡花括号的 `{…}`(**可含空格**,如 `{string: number}`)或第一个空白分隔的 token;
		// 说明 = 类型表达式之后的整段剩余文本(去掉首尾空白);没有类型 / 括号不闭合 → false(该行跳过,
		// 与旧口径一致:解析不了就不猜)。
		bool ParseFieldAnnotationRest(const std::string& rest, FieldAnnotation& annotation)
		{
			std::istringstream stream(rest);
			if (!(stream >> annotation.Name))
				return false;
			std::string remainder;
			std::getline(stream, remainder);
			const std::string text = TrimWhitespace(remainder);
			if (text.empty())
				return false;
			std::size_t cursor = 0;
			if (text.front() == '{')
			{
				int nesting = 0;
				bool closed = false;
				for (; cursor < text.size(); ++cursor)
				{
					if (text[cursor] == '{')
						++nesting;
					else if (text[cursor] == '}')
					{
						--nesting;
						if (nesting == 0)
						{
							++cursor;
							closed = true;
							break;
						}
						if (nesting < 0)
							break;
					}
				}
				if (!closed)
					return false;
			}
			else
			{
				cursor = text.find_first_of(" \t");
				if (cursor == std::string::npos)
					cursor = text.size();
			}
			annotation.TypeName = text.substr(0, cursor);
			annotation.Doc = TrimWhitespace(text.substr(cursor));
			return !annotation.TypeName.empty();
		}

		AnnotationList ParseFieldAnnotationListInternal(const std::string& text)
		{
			AnnotationList schema;
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
				FieldAnnotation annotation;
				if (!ParseFieldAnnotationRest(trimmed.substr(9), annotation))
					continue;
				if (std::any_of(schema.begin(), schema.end(),
						[&annotation](const auto& item) { return item.Name == annotation.Name; }))
					continue;   // 重复声明以第一次为准(与 ScriptProperties::SyncFromDeclarations 同口径)
				schema.push_back(std::move(annotation));
			}
			return schema;
		}

		std::unordered_map<std::string, std::string> ParseFieldAnnotationsInternal(const std::string& text)
		{
			std::unordered_map<std::string, std::string> schema;
			for (const FieldAnnotation& annotation : ParseFieldAnnotationListInternal(text))
				schema.emplace(annotation.Name, annotation.TypeName);
			return schema;
		}

		// W7-3:容器字节(不嵌源码)跳过 `---@field` 注解解析 → 字段类型走"旧值推断"回退;
		// 源码字节保持既有注解解析。判定只看前 4 字节 magic(与 LoadChunk 同一条判定)。
		// B 期:`---@class` 块(名字 / 基类 / 字段列表;顺序 = 源码顺序)。
		struct ClassAnnotation
		{
			std::string Name;
			std::string Base;
			AnnotationList Fields;
		};
		using ClassList = std::vector<ClassAnnotation>;

		// 一次解析的完整结果:扁平字段表(老口径,`ParseFieldAnnotations` 仍用它)+ 类块(嵌套表用)。
		struct ScriptAnnotations
		{
			AnnotationList Flat;
			ClassList Classes;
		};

		constexpr int kMaxObjectDepth = 4;      // 与 ScriptProperties 的护栏同口径(plan v2 §B)
		constexpr size_t kMaxObjectFields = 64;
		// C 期:注解里的集合嵌套层数上限 —— `{number}`/`{string: number}` = 1 层,`{{number}}` = 2 层;
		// 再深(如 `{{{number}}}`)→ 解析失败 → 只读摘要 + 诊断(不静默、不无限展开)。
		constexpr int kMaxCollectionDepth = 2;

		// C 期:一个类型表达式 —— 叶子(类型名)/ 数组 `{T}`(1 个参数)/ 映射 `{K: V}`(2 个参数)。
		struct TypeExpression
		{
			std::string Name;                      // 叶子类型名(容器时为空)
			std::vector<TypeExpression> Arguments; // 容器:1 = 元素;2 = (键, 值)
			bool IsContainer = false;
		};

		// `{…}` 里的顶层 `:` 分隔键与值(嵌套花括号里的 `:` 不算)。
		std::size_t FindTopLevelColon(const std::string& text)
		{
			int nesting = 0;
			for (std::size_t index = 0; index < text.size(); ++index)
			{
				const char character = text[index];
				if (character == '{')
					++nesting;
				else if (character == '}')
					--nesting;
				else if (character == ':' && nesting == 0)
					return index;
			}
			return std::string::npos;
		}

		// 类型表达式解析:`{T}` / `{K: V}`(递归)。解析失败(空 / 花括号不闭合 / 嵌套超护栏)→ false + reason,
		// 调用方降级为只读摘要并出诊断 —— 绝不把读不懂的写法当成某个默认类型。
		bool ParseTypeExpression(const std::string& text, int depth, TypeExpression& out, std::string& reason)
		{
			const std::string trimmed = TrimWhitespace(text);
			if (trimmed.empty())
			{
				reason = "empty type";
				return false;
			}
			if (trimmed.front() != '{')
			{
				if (trimmed.find_first_of(" \t{}:") != std::string::npos)
				{
					reason = "malformed type name '" + trimmed + "'";
					return false;
				}
				out.Name = trimmed;
				return true;
			}
			if (trimmed.size() < 2 || trimmed.back() != '}')
			{
				reason = "unbalanced braces in '" + trimmed + "'";
				return false;
			}
			if (depth >= kMaxCollectionDepth)
			{
				reason = "collection type nesting is deeper than the supported " +
					std::to_string(kMaxCollectionDepth) + " levels";
				return false;
			}
			out.IsContainer = true;
			const std::string inner = trimmed.substr(1, trimmed.size() - 2);
			const std::size_t colon = FindTopLevelColon(inner);
			if (colon == std::string::npos)
			{
				out.Arguments.resize(1);
				return ParseTypeExpression(inner, depth + 1, out.Arguments[0], reason);
			}
			out.Arguments.resize(2);
			return ParseTypeExpression(inner.substr(0, colon), depth + 1, out.Arguments[0], reason) &&
				ParseTypeExpression(inner.substr(colon + 1), depth + 1, out.Arguments[1], reason);
		}

		// 诊断里回显类型表达式的原始写法(`{string: number}` / `{{number}}`)。
		std::string TypeExpressionText(const TypeExpression& type)
		{
			if (!type.IsContainer)
				return type.Name;
			if (type.Arguments.size() == 1)
				return "{" + TypeExpressionText(type.Arguments[0]) + "}";
			return "{" + TypeExpressionText(type.Arguments[0]) + ": " + TypeExpressionText(type.Arguments[1]) + "}";
		}

		ClassList ParseClassAnnotationListInternal(const std::string& text)
		{
			ClassList classes;
			std::istringstream stream(text);
			std::string line;
			while (std::getline(stream, line))
			{
				const size_t start = line.find_first_not_of(" \t\r\n");
				if (start == std::string::npos)
					continue;
				const std::string trimmed = line.substr(start);
				if (trimmed.rfind("---@class", 0) == 0)
				{
					std::istringstream rest(trimmed.substr(9));
					ClassAnnotation entry;
					if (!(rest >> entry.Name))
						continue;
					std::string token;
					if (rest >> token)
					{
						if (token == ":")
							rest >> entry.Base;
						else if (token.size() > 1 && token.front() == ':')
							entry.Base = token.substr(1);
					}
					classes.push_back(std::move(entry));
					continue;
				}
				if (trimmed.rfind("---@field", 0) != 0 || classes.empty())
					continue;
				FieldAnnotation annotation;
				if (!ParseFieldAnnotationRest(trimmed.substr(9), annotation))
					continue;
				ClassAnnotation& owner = classes.back();
				if (std::any_of(owner.Fields.begin(), owner.Fields.end(),
						[&annotation](const FieldAnnotation& item) { return item.Name == annotation.Name; }))
					continue;   // 重复声明以第一次为准(与扁平解析同口径)
				owner.Fields.push_back(std::move(annotation));
			}
			return classes;
		}

		ScriptAnnotations ParseScriptAnnotationsInternal(const std::string& text)
		{
			ScriptAnnotations parsed;
			parsed.Flat = ParseFieldAnnotationListInternal(text);
			parsed.Classes = ParseClassAnnotationListInternal(text);
			return parsed;
		}

		// 根字段表:优先 `: WorldScript` 的类(脚本类);否则第一个类;一个类都没有 → 扁平表(老口径)。
		const AnnotationList& RootAnnotationsOf(const ScriptAnnotations& parsed)
		{
			for (const ClassAnnotation& entry : parsed.Classes)
				if (entry.Base == "WorldScript")
					return entry.Fields;
			if (!parsed.Classes.empty())
				return parsed.Classes.front().Fields;
			return parsed.Flat;
		}

		const ClassAnnotation* FindClassInternal(const ClassList& classes, const std::string& name)
		{
			for (const ClassAnnotation& entry : classes)
				if (entry.Name == name)
					return &entry;
			return nullptr;
		}

		ScriptAnnotations ParseAnnotationsForBytes(const std::vector<uint8_t>& bytes)
		{
			if (Asset::ScriptArtifact::IsArtifactBytes(bytes.data(), bytes.size()))
				return {};
			return ParseScriptAnnotationsInternal(std::string(bytes.begin(), bytes.end()));
		}

		// VEC-A1(D2):VM 与绑定上下文都活着才做 userdata 的装箱/解包。
		// 纯工具或 VM 不可用(离线声明解析、Shutdown 之后)时保持旧行为:
		// 读值失败 → 该字段跳过;写值返回 Nil → 调用方跳过,**绝不把脚本字段清成 nil**。
		ScriptBindingContext* BindingContextIfAvailable()
		{
			return (s_Vm && s_Bindings) ? s_Bindings.get() : nullptr;
		}

		// V1(2026-09-26):注解类型名 → schema 值类型 —— **与值无关的固定映射**。
		//   number       → Float(两侧统一:编辑器和引擎都是 Float;P1-2 的"编辑值被判类型变了"由此消除)
		//   integer/int  → Int32,boolean/bool → Bool,string → String
		//   vec2/vec3/vec4 → Vec2/Vec3/Vec4(Luau 侧是 userdata;读写都走绑定上下文,值按**拷贝**进出)
		// 未知类型名 → None(跳过该字段 + 诊断)。
		Schema::Kind AnnotationToKindInternal(const std::string& typeName)
		{
			if (typeName == "number") return Schema::Kind::Float;
			if (typeName == "integer" || typeName == "int") return Schema::Kind::Int32;
			if (typeName == "boolean" || typeName == "bool") return Schema::Kind::Bool;
			if (typeName == "string") return Schema::Kind::String;
			if (typeName == "vec2") return Schema::Kind::Vec2;
			if (typeName == "vec3") return Schema::Kind::Vec3;
			if (typeName == "vec4") return Schema::Kind::Vec4;
			return Schema::Kind::None;
		}

		// 没有注解的字段(容器脚本 / 未写注解的表项):按值推断,与旧口径一致。
		Schema::Kind InferKindFromValueInternal(const ScriptValue& value)
		{
			double number = 0.0;
			if (value.AsNumber(&number))
			{
				if (std::isfinite(number) && number == std::floor(number) &&
					number >= (std::numeric_limits<int32_t>::min)() && number <= (std::numeric_limits<int32_t>::max)())
					return Schema::Kind::Int32;
				return Schema::Kind::Float;
			}
			if (value.IsBoolean()) return Schema::Kind::Bool;
			if (value.IsString()) return Schema::Kind::String;
			// VEC-A1(D2):脚本表里的向量 userdata 也要能进属性表(未写注解的现状保持)。
			// 只有能安全问到 userdata 类型名时才推断(无 VM / 非本 VM 的值 → 维持旧行为 None,不猜)。
			if (value.IsUserdata())
			{
				if (ScriptBindingContext* bindings = BindingContextIfAvailable())
				{
					if (bindings->IsUserdataOfType("vec2", value)) return Schema::Kind::Vec2;
					if (bindings->IsUserdataOfType("vec3", value)) return Schema::Kind::Vec3;
					if (bindings->IsUserdataOfType("vec4", value)) return Schema::Kind::Vec4;
				}
				return Schema::Kind::None;
			}
			return Schema::Kind::None;
		}

		// 脚本值 → 属性值:按声明的类型严格转换;失败返回 false 且不改 *out。
		// 兼容只放宽 number 域:声明 Int32 要求整数值且在 int32 范围内;Float 接受任何 number;
		// Bool/String 严格同类型。
		bool ReadPropertyValueInternal(const ScriptValue& value, Schema::Kind kind, Schema::Value* out)
		{
			if (!out)
				return false;
			switch (kind)
			{
				case Schema::Kind::Int32:
				{
					double number = 0.0;
					if (!value.AsNumber(&number) || !std::isfinite(number) || number != std::floor(number) ||
						number < (std::numeric_limits<int32_t>::min)() || number > (std::numeric_limits<int32_t>::max)())
						return false;
					*out = static_cast<int32_t>(number);
					return true;
				}
				case Schema::Kind::Float:
				{
					double number = 0.0;
					if (!value.AsNumber(&number))
						return false;
					*out = static_cast<float>(number);
					return true;
				}
				case Schema::Kind::Bool:
				{
					bool boolean = false;
					if (!value.AsBool(&boolean))
						return false;
					*out = boolean;
					return true;
				}
				case Schema::Kind::String:
				{
					std::string text;
					if (!value.AsString(&text))
						return false;
					*out = std::move(text);
					return true;
				}
				// VEC-A1(D2):Luau 向量是 userdata —— 按类型名解包后**拷贝**成 glm 值,
				// 绝不把脚本侧指针存进属性表(userdata 随脚本表/GC 释放)。
				case Schema::Kind::Vec2:
				{
					ScriptBindingContext* bindings = BindingContextIfAvailable();
					glm::vec2* source = nullptr;
					if (!bindings || !bindings->Unwrap("vec2", value, &source) || !source)
						return false;
					*out = *source;
					return true;
				}
				case Schema::Kind::Vec3:
				{
					ScriptBindingContext* bindings = BindingContextIfAvailable();
					glm::vec3* source = nullptr;
					if (!bindings || !bindings->Unwrap("vec3", value, &source) || !source)
						return false;
					*out = *source;
					return true;
				}
				case Schema::Kind::Vec4:
				{
					ScriptBindingContext* bindings = BindingContextIfAvailable();
					glm::vec4* source = nullptr;
					if (!bindings || !bindings->Unwrap("vec4", value, &source) || !source)
						return false;
					*out = *source;
					return true;
				}
				default:
					return false;
			}
		}

		// 从"活表"(热重载前的旧脚本表)读一个自有字段:只有 rawget 非 nil 才算存在,
		// 值按新声明的类型转换。缺失/类型不符 → false(回退场景保存值)。
		bool ReadLiveFieldInternal(const ScriptTableRef& table, const std::string& name, Schema::Kind kind,
			Schema::Value* out)
		{
			if (!table.IsValid() || !table.HasField(name.c_str()))
				return false;
			return ReadPropertyValueInternal(table.GetField(name.c_str()), kind, out);
		}

		// VEC-A1(D2):glm 向量 → vecN userdata(写回脚本表 / 热重载迁移用)。
		// VM 不可用时返回 Nil,调用方跳过(不清脚本字段)。
		template <typename T>
		ScriptValue NewVectorUserdata(const char* typeName, const T& vector)
		{
			ScriptBindingContext* bindings = BindingContextIfAvailable();
			if (!bindings)
				return ScriptValue::Nil();
			ScriptValue result = bindings->NewUserdata(typeName);
			T* target = nullptr;
			if (!bindings->Unwrap(typeName, result, &target) || !target)
				return ScriptValue::Nil();
			new (target) T(vector);
			return result;
		}

		// 属性值 → 脚本值(写回脚本表用);空值(monostate)返回 Nil,调用方跳过。
		ScriptValue ToScriptValueInternal(const Schema::Value& value)
		{
			if (const bool* boolean = std::get_if<bool>(&value)) return ScriptValue::Boolean(*boolean);
			if (const int32_t* number = std::get_if<int32_t>(&value)) return ScriptValue::Number(static_cast<double>(*number));
			if (const float* number = std::get_if<float>(&value)) return ScriptValue::Number(static_cast<double>(*number));
			if (const double* number = std::get_if<double>(&value)) return ScriptValue::Number(*number);
			if (const int64_t* number = std::get_if<int64_t>(&value)) return ScriptValue::Number(static_cast<double>(*number));
			if (const uint32_t* number = std::get_if<uint32_t>(&value)) return ScriptValue::Number(static_cast<double>(*number));
			if (const std::string* text = std::get_if<std::string>(&value)) return ScriptValue::String(*text);
			if (const glm::vec2* vector = std::get_if<glm::vec2>(&value)) return NewVectorUserdata("vec2", *vector);
			if (const glm::vec3* vector = std::get_if<glm::vec3>(&value)) return NewVectorUserdata("vec3", *vector);
			if (const glm::vec4* vector = std::get_if<glm::vec4>(&value)) return NewVectorUserdata("vec4", *vector);
			return ScriptValue::Nil();
		}

		// 脚本返回表的**自有字符串键**(跳过 `_` 前缀与 `entity`),顺序 = 表遍历顺序。
		std::vector<std::string> CollectOwnFieldNames(const ScriptTableRef& table)
		{
			std::vector<std::string> names;
			if (!table.IsValid())
				return names;   // VM 不可用 / 表无效:声明退回"只有注解"的形态
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
			return names;
		}

		// C 期:一条"自有键"条目(名字 / 数值键 / 值)。pairs 的键可能是 string 或 number,
		// 名字统一取 tostring(与检视器行标签、存档键一致)。
		struct OwnEntry
		{
			std::string Name;
			double Number = 0.0;
			bool HasNumber = false;
			ScriptValue Value;
		};

		// 收集目标表的自有键条目(numeric = true → number 键;false → string 键)。
		// 顺序 = pairs 的遍历顺序;需要顺序的地方(数组 = 下标 1..n、结构化表 = 注解顺序)各自对齐。
		std::vector<OwnEntry> CollectOwnEntries(const ScriptTableRef& table, bool numeric)
		{
			std::vector<OwnEntry> entries;
			if (!table.IsValid())
				return entries;
			ScriptValue result;
			std::string error;
			const ScriptValue args[] = { table.ToValue(), ScriptValue::Boolean(numeric) };
			if (!s_CollectEntries.Call(args, 2, &result, &error))
				throw std::runtime_error(error);
			ScriptTableRef rows;
			if (!result.AsTable(&rows))
				return entries;
			for (const ScriptValue& row : rows.GetArray())
			{
				ScriptTableRef fields;
				if (!row.AsTable(&fields))
					continue;
				OwnEntry entry;
				std::string name;
				if (fields.GetField("name").AsString(&name))
					entry.Name = std::move(name);
				double number = 0.0;
				if (fields.GetField("number").AsNumber(&number))
				{
					entry.Number = number;
					entry.HasNumber = true;
				}
				entry.Value = fields.GetField("value");
				entries.push_back(std::move(entry));
			}
			return entries;
		}

		// C 期:表的形态判定(未注解字段的类型推导用)——
		//   Empty          无自有 string/number 键 → 空表(只读摘要);
		//   StringKeys     只有字符串键 → 结构化表(命名子行,递归,带初值);
		//   ArrayLike      恰好是连续整数键 1..n(Length() = 连续段,且数字键个数相等)→ 数组;
		//   Opaque         数字键不连续 / 数字与字符串混合 → 只读摘要 + 诊断(顺序不可靠,不猜)。
		enum class TableShape { Empty, StringKeys, ArrayLike, Opaque };

		TableShape ClassifyTable(const ScriptTableRef& table, std::vector<ScriptValue>& elements)
		{
			elements.clear();
			if (!table.IsValid())
				return TableShape::Empty;
			const std::size_t numericKeys = CollectOwnEntries(table, true).size();
			const std::size_t stringKeys = CollectOwnEntries(table, false).size();
			if (numericKeys == 0 && stringKeys == 0)
				return TableShape::Empty;
			if (numericKeys == 0)
				return TableShape::StringKeys;
			if (stringKeys != 0)
				return TableShape::Opaque;
			const std::size_t length = table.Length();   // 1,2,3… 走到第一个 nil(整数连续语义)
			if (length == 0 || length != numericKeys)
				return TableShape::Opaque;
			elements = table.GetArray();
			return TableShape::ArrayLike;
		}

		// V1:脚本表 + 注解 → 有序声明表(name / Schema::Kind / Doc / 默认值)。
		//   顺序 = 注解顺序(先声明先显示)→ 表里其余字段顺序;
		//   注解:类型走固定映射(number→Float);字段在表里时按声明类型取默认值(类型不符 → 跳过 + 诊断);
		//         字段不在表里也保留声明(编辑器要显示),默认值 = monostate(未设,不写零值);
		//   无注解字段(容器脚本 / 未写注解的表项):按值推断类型,默认值 = 表里的值。
		// B 期 v3:未注解字段的类型推导 —— 叶子沿用旧口径;字符串键的表 → Struct(命名子行,递归,带初值);
		// C 期:连续整数键 1..n 的表 → Array(元素同型才可编辑;异质/不连续/混合/空表 → 只读摘要 + 诊断)。
		bool InferDeclarationFromValueInternal(const ScriptValue& value, const std::string& name, int depth,
			const std::string& scriptPath, std::vector<std::string>* diagnostics,
			ScriptProperties::Declaration& out)
		{
			out = ScriptProperties::Declaration {};
			out.Name = name;
			const Schema::Kind kind = InferKindFromValueInternal(value);
			if (kind != Schema::Kind::None)
			{
				out.Type = kind;
				return ReadPropertyValueInternal(value, kind, &out.Default);
			}
			ScriptTableRef nested;
			if (!value.AsTable(&nested) || !nested.IsValid())
				return false;

			const auto report = [&](const std::string& detail)
			{
				if (diagnostics)
					diagnostics->push_back("[script] " + scriptPath + ": field '" + name + "' " + detail);
			};
			// 只读摘要:看得到、不进存档(裸 table / 空表 / 不支持的键形态 / 超护栏共用)。
			const auto summary = [&](const std::string& detail, ScriptPropertyCollection collection)
			{
				out.Type = Schema::Kind::Object;
				out.Collection = collection;
				out.TypeName = "table";
				out.ElementKind = Schema::Kind::None;
				out.ReadOnly = true;
				out.Fields.clear();
				report(detail);
			};

			std::vector<ScriptValue> elements;
			const TableShape shape = ClassifyTable(nested, elements);
			if (shape == TableShape::Empty)
			{
				summary("is an empty table; the field is shown read-only",
					ScriptPropertyCollection::Struct);
				return true;
			}
			if (shape == TableShape::Opaque)
			{
				summary("has non-consecutive numeric keys or mixes numeric and string keys; "
					"the field is shown read-only", ScriptPropertyCollection::Struct);
				return true;
			}
			if (shape == TableShape::StringKeys)
			{
				out.Type = Schema::Kind::Object;
				out.Collection = ScriptPropertyCollection::Struct;
				out.TypeName = "table";
				if (depth >= kMaxObjectDepth)
				{
					summary("nests deeper than the supported " + std::to_string(kMaxObjectDepth) +
						" levels; the field is shown read-only", ScriptPropertyCollection::Struct);
					return true;
				}
				const std::vector<std::string> childNames = CollectOwnFieldNames(nested);
				std::unordered_set<std::string> visited;
				for (const std::string& child : childNames)
				{
					if (visited.count(child) || child.empty() || child[0] == '_')
						continue;
					if (out.Fields.size() >= kMaxObjectFields)
						break;
					visited.insert(child);
					ScriptProperties::Declaration field;
					if (InferDeclarationFromValueInternal(nested.GetField(child.c_str()), child, depth + 1,
							scriptPath, diagnostics, field))
						out.Fields.push_back(std::move(field));
				}
				if (out.Fields.empty())
				{
					// 只有下划线/entity 一类被跳过的键 → 与空表同一落点(只读摘要)。
					summary("has no exposable fields; the field is shown read-only",
						ScriptPropertyCollection::Struct);
				}
				return true;
			}

			// ArrayLike:连续整数键 1..n。元素同型(数值 Int32/Float 之间按 Float 提升)才可编辑;
			// 元素本身是表 → 递归推断(元组/嵌套数组);异质 → 只读摘要 + 诊断。
			out.Type = Schema::Kind::Object;
			out.Collection = ScriptPropertyCollection::Array;
			out.TypeName = "array";
			if (depth >= kMaxObjectDepth)
			{
				summary("nests deeper than the supported " + std::to_string(kMaxObjectDepth) +
					" levels; the field is shown read-only", ScriptPropertyCollection::Array);
				return true;
			}
			if (elements.size() > kMaxObjectFields)
			{
				summary("has more than " + std::to_string(kMaxObjectFields) +
					" elements; the field is shown read-only", ScriptPropertyCollection::Array);
				return true;
			}
			std::size_t tableElements = 0;
			Schema::Kind elementKind = Schema::Kind::None;
			bool heterogeneous = false;
			for (const ScriptValue& element : elements)
			{
				const Schema::Kind elementValueKind = InferKindFromValueInternal(element);
				if (elementValueKind == Schema::Kind::None)
				{
					ScriptTableRef probe;
					if (element.AsTable(&probe) && probe.IsValid())
					{
						++tableElements;
						continue;
					}
					heterogeneous = true;   // 函数 / userdata / 其它非表值 → 不作为数组元素类型
					break;
				}
				if (elementKind == Schema::Kind::None)
					elementKind = elementValueKind;
				else if (elementKind != elementValueKind)
				{
					const bool bothNumeric =
						(elementKind == Schema::Kind::Int32 || elementKind == Schema::Kind::Float) &&
						(elementValueKind == Schema::Kind::Int32 || elementValueKind == Schema::Kind::Float);
					if (bothNumeric)
						elementKind = Schema::Kind::Float;   // 整数与浮点混排 → 统一 Float
					else
					{
						heterogeneous = true;
						break;
					}
				}
			}
			if (!heterogeneous && tableElements != 0 && tableElements != elements.size())
				heterogeneous = true;   // 表元素与叶子元素混排
			if (heterogeneous)
			{
				summary("looks like an array but its elements have different value types; "
					"the field is shown read-only", ScriptPropertyCollection::Array);
				return true;
			}
			if (tableElements == elements.size())
			{
				// 元素本身是集合(如 `{{number}}` 的值侧):逐元素递归,任一元素读不出来 → 整条只读。
				out.ElementKind = Schema::Kind::Object;
				for (std::size_t index = 0; index < elements.size(); ++index)
				{
					ScriptProperties::Declaration element;
					if (!InferDeclarationFromValueInternal(elements[index], std::to_string(index + 1),
							depth + 1, scriptPath, diagnostics, element) || element.ReadOnly)
					{
						summary("is an array of tables that could not be inferred element by element; "
							"the field is shown read-only", ScriptPropertyCollection::Array);
						return true;
					}
					out.Fields.push_back(std::move(element));
				}
				return true;
			}
			out.ElementKind = elementKind;
			for (std::size_t index = 0; index < elements.size(); ++index)
			{
				ScriptProperties::Declaration element;
				element.Name = std::to_string(index + 1);   // 数组行名 = 下标字符串(1 起)
				element.Type = elementKind;
				if (!ReadPropertyValueInternal(elements[index], elementKind, &element.Default))
				{
					summary("looks like an array but its elements have different value types; "
						"the field is shown read-only", ScriptPropertyCollection::Array);
					return true;
				}
				out.Fields.push_back(std::move(element));
			}
			return true;
		}

		// C 期:值 + 类型表达式 → 声明的结果分类(决定顶层是"跳过"还是"只读摘要" + 诊断措辞)。
		enum class ValueBuild
		{
			Ok,
			UnsupportedType,   // 类型名不认识(元素/键/字段)
			Mismatch,          // 类型认识,但脚本表里的值不是这个类型
			BadShape,          // 表形态读不出来(不连续 / 混合键 / 嵌套超护栏 / 元素异质)
		};

		std::vector<ScriptProperties::Declaration> BuildDeclarations(const ScriptTableRef& table,
			const ScriptAnnotations& annotations, const std::string& scriptPath, std::vector<std::string>* diagnostics)
		{
			const std::vector<std::string> names = CollectOwnFieldNames(table);
			const auto skipName = [](const std::string& name)
			{
				return name.empty() || name[0] == '_' || name == "entity";
			};

			// C 期:类型表达式 + 脚本值 → 一条声明(不含 Name/Doc,由调用方填)。
			// 叶子沿用老口径(固定映射 + 从脚本表读默认值);容器 = 数组/映射(元素/键值行递归);
			// `---@class` = B 期结构化表(子字段按注解顺序递归,坏子字段跳过)。
			std::function<ValueBuild(const TypeExpression&, const ScriptValue&, bool, int,
				std::vector<std::string>&, const std::string&, ScriptProperties::Declaration&, std::string&)> buildValue;
			// 一条注解字段 → 一条声明(Name/Doc + 诊断)。
			std::function<void(const FieldAnnotation&, const ScriptTableRef&, const std::vector<std::string>&,
				int, std::vector<std::string>&, ScriptProperties::Declaration&)> buildOne;

			buildValue = [&](const TypeExpression& type, const ScriptValue& value, bool hasValue, int depth,
				std::vector<std::string>& classStack, const std::string& fieldPath,
				ScriptProperties::Declaration& out, std::string& reason) -> ValueBuild
			{
				out = ScriptProperties::Declaration {};
				const bool usableValue = hasValue && !value.IsNil();
				if (!type.IsContainer)
				{
					const Schema::Kind kind = AnnotationToKindInternal(type.Name);
					if (kind != Schema::Kind::None)
					{
						out.Type = kind;
						if (usableValue && !ReadPropertyValueInternal(value, kind, &out.Default))
							return ValueBuild::Mismatch;
						return ValueBuild::Ok;
					}
					if (type.Name == "table")
					{
						// E3①(2026-09-27 用户口径:第 45 行 `ExtraInfo` 这种裸 table 要"全部进面板"):
						// 用脚本表里的**实际值**递归推断结构 —— 字符串键 → 结构化行(可展开、可编辑、
						// 随场景保存),连续整数键 1..n → 数组行(`+`/`-` 可增删),与 `---@class` 走
						// 同一条渲染/存档路径(Collection/Children/ElementKind)。
						// 推不出来(空表 / 键不连续或混合 / 元素异质 / 超护栏 / 没有值)→ 保持旧口径:
						// 只读摘要(看得到、不进存档),诊断由推断函数给出。
						if (usableValue)
						{
							ScriptProperties::Declaration inferred;
							if (InferDeclarationFromValueInternal(value, fieldPath, depth, scriptPath,
									diagnostics, inferred) && !inferred.ReadOnly)
							{
								out = std::move(inferred);
								out.Name.clear();   // buildValue 的契约:名字/说明由调用方(buildOne)恢复
								out.Doc.clear();
								return ValueBuild::Ok;
							}
						}
						out.Type = Schema::Kind::Object;
						out.Collection = ScriptPropertyCollection::Struct;
						out.TypeName = "table";
						out.ReadOnly = true;
						return ValueBuild::Ok;
					}
					const ClassAnnotation* nested = FindClassInternal(annotations.Classes, type.Name);
					if (!nested)
					{
						reason = type.Name;
						return ValueBuild::UnsupportedType;
					}
					out.Type = Schema::Kind::Object;
					out.Collection = ScriptPropertyCollection::Struct;
					out.TypeName = nested->Name;
					if (depth >= kMaxObjectDepth)
					{
						if (diagnostics)
							diagnostics->push_back("[script] " + scriptPath + ": field '" + fieldPath +
								"' nests deeper than the supported " + std::to_string(kMaxObjectDepth) +
								" levels; the field is shown read-only");
						out.ReadOnly = true;
						return ValueBuild::Ok;
					}
					if (std::find(classStack.begin(), classStack.end(), nested->Name) != classStack.end())
					{
						if (diagnostics)
							diagnostics->push_back("[script] " + scriptPath + ": class '" + nested->Name +
								"' forms a reference cycle; the field is shown read-only");
						out.ReadOnly = true;
						return ValueBuild::Ok;
					}
					ScriptTableRef nestedTable;
					if (usableValue)
						value.AsTable(&nestedTable);   // 不是表 → 子字段全部保持"未设"(不猜、不写零值)
					const std::vector<std::string> nestedNames = CollectOwnFieldNames(nestedTable);
					classStack.push_back(nested->Name);
					std::unordered_set<std::string> nestedVisited;
					for (const FieldAnnotation& child : nested->Fields)
					{
						if (nestedVisited.count(child.Name) || skipName(child.Name))
							continue;
						if (out.Fields.size() >= kMaxObjectFields)
							break;   // 单层子字段上限(护栏)
						nestedVisited.insert(child.Name);
						ScriptProperties::Declaration childDeclaration;
						buildOne(child, nestedTable, nestedNames, depth + 1, classStack, childDeclaration);
						if (childDeclaration.Type == Schema::Kind::None)
							continue;
						out.Fields.push_back(std::move(childDeclaration));
					}
					classStack.pop_back();
					return ValueBuild::Ok;
				}

				// ---- C 期:数组 / 映射 ----
				const bool map = type.Arguments.size() == 2;
				out.Type = Schema::Kind::Object;
				out.Collection = map ? ScriptPropertyCollection::Map : ScriptPropertyCollection::Array;
				out.TypeName = map ? "map" : "array";
				const TypeExpression& element = type.Arguments.back();
				if (map)
				{
					const TypeExpression& key = type.Arguments.front();
					if (key.IsContainer)
					{
						reason = "a map key cannot be a collection";
						return ValueBuild::BadShape;
					}
					const Schema::Kind keyKind = AnnotationToKindInternal(key.Name);
					if (keyKind != Schema::Kind::String && keyKind != Schema::Kind::Int32 && keyKind != Schema::Kind::Float)
					{
						reason = key.Name;
						return ValueBuild::UnsupportedType;
					}
					out.KeyKind = keyKind;
				}
				ScriptTableRef containerTable;
				if (usableValue)
				{
					if (!value.AsTable(&containerTable) || !containerTable.IsValid())
					{
						reason = "the script table holds a different value type";
						return ValueBuild::Mismatch;
					}
				}
				// 键/元素行:元素是叶子时按 ElementKind 逐行读值;元素本身是集合时递归。
				const Schema::Kind elementKind = element.IsContainer
					? Schema::Kind::Object : AnnotationToKindInternal(element.Name);
				if (elementKind == Schema::Kind::None)
				{
					reason = element.Name;
					return ValueBuild::UnsupportedType;
				}
				out.ElementKind = elementKind;
				// 没有 VM(读不到默认表)/ 脚本表里没有这个字段 → 元素行未知:合并时保留场景里已有的元素值。
				out.FieldsUnknown = !usableValue;
				if (map)
				{
					// 键类型不匹配的键(例如声明 {string: number} 但表里有数字键)不能静默丢 —— 降级只读。
					const std::vector<OwnEntry> otherKeys = CollectOwnEntries(containerTable, out.KeyKind == Schema::Kind::String);
					if (!otherKeys.empty())
					{
						reason = "the script table holds keys of a different type";
						return ValueBuild::BadShape;
					}
					for (const OwnEntry& entry : CollectOwnEntries(containerTable, out.KeyKind != Schema::Kind::String))
					{
						if (out.Fields.size() >= kMaxObjectFields)
						{
							reason = "the map has more than " + std::to_string(kMaxObjectFields) + " entries";
							return ValueBuild::BadShape;
						}
						ScriptProperties::Declaration child;
						if (elementKind == Schema::Kind::Object)
						{
							std::string nestedReason;
							const ValueBuild nested = buildValue(element, entry.Value, true, depth + 1, classStack,
								fieldPath + "[" + entry.Name + "]", child, nestedReason);
							if (nested != ValueBuild::Ok)
							{
								reason = nestedReason;
								return nested;
							}
						}
						else
						{
							child.Type = elementKind;
							if (!ReadPropertyValueInternal(entry.Value, elementKind, &child.Default))
							{
								reason = "the script table holds a different value type in the map";
								return ValueBuild::Mismatch;
							}
						}
						child.Name = entry.Name;   // 行名 = 键(mapping 行标签;nested 分支在上面会重置 Name)
						out.Fields.push_back(std::move(child));
					}
					return ValueBuild::Ok;
				}
				std::vector<ScriptValue> elements;
				const TableShape shape = ClassifyTable(containerTable, elements);
				if (shape == TableShape::StringKeys || shape == TableShape::Opaque)
				{
					reason = shape == TableShape::Opaque
						? "the script table holds non-consecutive numeric keys or mixed keys"
						: "the script table holds a table with string keys where the annotation declares an array";
					return ValueBuild::BadShape;
				}
				if (elements.size() > kMaxObjectFields)
				{
					reason = "the array has more than " + std::to_string(kMaxObjectFields) + " elements";
					return ValueBuild::BadShape;
				}
				for (std::size_t index = 0; index < elements.size(); ++index)
				{
					ScriptProperties::Declaration child;
					if (elementKind == Schema::Kind::Object)
					{
						std::string nestedReason;
						const ValueBuild nested = buildValue(element, elements[index], true, depth + 1, classStack,
							fieldPath + "[" + std::to_string(index + 1) + "]", child, nestedReason);
						if (nested != ValueBuild::Ok)
						{
							reason = nestedReason;
							return nested;
						}
					}
					else
					{
						child.Type = elementKind;
						if (!ReadPropertyValueInternal(elements[index], elementKind, &child.Default))
						{
							reason = "the script table holds a different value type in the array";
							return ValueBuild::Mismatch;
						}
					}
					child.Name = std::to_string(index + 1);   // 数组行名 = 下标字符串(1 起;nested 分支会重置 Name)
					out.Fields.push_back(std::move(child));
				}
				return ValueBuild::Ok;
			};

			buildOne = [&](const FieldAnnotation& annotation, const ScriptTableRef& ownerTable,
				const std::vector<std::string>& ownerNames, int depth, std::vector<std::string>& classStack,
				ScriptProperties::Declaration& out)
			{
				out = ScriptProperties::Declaration {};
				out.Name = annotation.Name;
				out.Doc = annotation.Doc;
				// 只读**自有字段**(rawget 口径):`__index` 继承/错误元表不能在这里被触发
				// (旧口径同此 —— 否则"加载期就该报的错"会提前在这里以别的形态炸掉)。
				const bool hasOwnField = std::find(ownerNames.begin(), ownerNames.end(), annotation.Name) != ownerNames.end();
				const ScriptValue fieldValue = hasOwnField ? ownerTable.GetField(annotation.Name.c_str()) : ScriptValue {};

				TypeExpression expression;
				std::string parseError;
				if (!ParseTypeExpression(annotation.TypeName, 0, expression, parseError))
				{
					if (diagnostics)
						diagnostics->push_back("[script] " + scriptPath + ": field '" + annotation.Name +
							"' declares unsupported type '" + annotation.TypeName + "' (" + parseError +
							"); the field is shown read-only");
					out.Type = Schema::Kind::Object;
					out.Collection = ScriptPropertyCollection::Struct;
					out.TypeName = "table";
					out.ReadOnly = true;
					return;
				}

				std::string reason;
				const ValueBuild built = buildValue(expression, fieldValue, hasOwnField, depth, classStack,
					annotation.Name, out, reason);
				// buildValue 会整条重置 out(它是"不含名字/说明"的构造器)→ 这里把注解的两段恢复回来。
				out.Name = annotation.Name;
				out.Doc = annotation.Doc;
				if (built == ValueBuild::Ok)
					return;
				if (!expression.IsContainer)
				{
					// 叶子/类名:沿用旧口径 —— 读不出来就**不进属性表**(与 A/B 期一致)。
					if (diagnostics)
					{
						if (built == ValueBuild::Mismatch)
							diagnostics->push_back("[script] " + scriptPath + ": field '" + annotation.Name +
								"' declares type '" + annotation.TypeName +
								"' but the script table holds a different value type; "
								"the field is not exposed as a script property");
						else
							diagnostics->push_back("[script] " + scriptPath + ": field '" + annotation.Name +
								"' declares unsupported type '" + annotation.TypeName +
								"' (expected number/integer/boolean/string/vec2/vec3/vec4/table, "
								"`{T}`/`{K: V}` for arrays/maps, or a ---@class declared in this file); "
								"the field is not exposed as a script property");
					}
					out.Type = Schema::Kind::None;
					return;
				}
				// C 期:数组/映射读不出来 → **只读摘要**(看得到、不进存档),诊断必须说明原因。
				if (diagnostics)
				{
					const std::string detail = built == ValueBuild::UnsupportedType
						? "' declares unsupported type '" + annotation.TypeName + "' (" + reason + ")"
						: "' declares type '" + annotation.TypeName + "' but " + reason;
					diagnostics->push_back("[script] " + scriptPath + ": field '" + annotation.Name + detail +
						"; the field is shown read-only");
				}
				out = ScriptProperties::Declaration {};
				out.Name = annotation.Name;
				out.Doc = annotation.Doc;
				out.Type = Schema::Kind::Object;
				out.Collection = expression.Arguments.size() == 2
					? ScriptPropertyCollection::Map : ScriptPropertyCollection::Array;
				out.TypeName = "table";
				out.ReadOnly = true;
			};

			// 注解字段:按注解顺序(先声明先显示);重复声明以第一次为准。
			const auto buildList = [&](const ScriptTableRef& ownerTable, const AnnotationList& list, int depth,
				std::vector<std::string>& classStack, std::unordered_set<std::string>& visited,
				std::vector<ScriptProperties::Declaration>& out)
			{
				const std::vector<std::string> ownerNames = CollectOwnFieldNames(ownerTable);
				for (const FieldAnnotation& annotation : list)
				{
					if (visited.count(annotation.Name) || skipName(annotation.Name))
						continue;
					if (out.size() >= kMaxObjectFields)
						break;   // 单层子字段上限(护栏)
					visited.insert(annotation.Name);
					ScriptProperties::Declaration declaration;
					buildOne(annotation, ownerTable, ownerNames, depth, classStack, declaration);
					if (declaration.Type == Schema::Kind::None)
						continue;
					out.push_back(std::move(declaration));
				}
			};

			std::vector<ScriptProperties::Declaration> declared;
			std::unordered_set<std::string> visited;
			std::vector<std::string> classStack;
			// 表里没有声明的字段:声明仍成立,默认值保持 monostate(未设)—— 绝不写类型零值。
			buildList(table, RootAnnotationsOf(annotations), 0, classStack, visited, declared);
			for (const std::string& name : names)
			{
				if (visited.count(name) || skipName(name))
					continue;
				visited.insert(name);
				const ScriptValue value = table.GetField(name.c_str());
				ScriptProperties::Declaration declaration;
				if (!InferDeclarationFromValueInternal(value, name, 0, scriptPath, diagnostics, declaration))
					continue;
				declared.push_back(std::move(declaration));
			}
			return declared;
		}

		// 新脚本 → 属性表同步(Luau 的所有加载路径唯一的入口):
		//   1. 声明表 = 注解顺序 → 表序(含 Doc 与脚本里的默认值,见 BuildDeclarations);
		//   2. liveTable(只有热重载传)里同名同类型的自有值覆盖旧值(运行期 self.X=... 的真实状态);
		//   3. ScriptProperties::SyncFromDeclarations 合并:同名同类型保留旧值(场景保存值 /
		//      编辑器改过的值),新字段/类型变化取声明里的默认值或保持"未设"。
		// 优先级:活表 > 场景保存值 > 脚本默认值(NULL 保持未设,绝不写零值)。


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
			owner.Component = 0;
			owner.Generation = generation;
			return owner;
		}

		// C 期:按键写值 —— 字符串键走 ScriptTableRef::SetField;数字键(映射的 number/integer 键)
		// 没有对应的 C++ 脚本表 API,走受保护的小 helper(受保护的调用失败 → 抛错,与 SetField 同落点)。
		void SetScriptTableKey(const ScriptTableRef& table, const std::string& name, Schema::Kind keyKind,
			const ScriptValue& value)
		{
			if (keyKind == Schema::Kind::String)
			{
				if (!table.SetField(name.c_str(), value))
					throw std::logic_error("Cannot assign script map key '" + name + "'");
				return;
			}
			double number = 0.0;
			try { number = std::stod(name); }
			catch (...) { throw std::logic_error("Invalid numeric map key '" + name + "'"); }
			ScriptValue result;
			std::string error;
			const ScriptValue args[] = { table.ToValue(), ScriptValue::Number(number), value };
			if (!s_SetIndex.Call(args, 3, &result, &error))
				throw std::runtime_error(error.empty()
					? "Cannot assign script map key '" + name + "'" : error);
		}

		// 属性表 → 脚本表(实例创建 / 热重载交换前写入)。空值字段跳过:保留脚本自己的默认值。
		// B/C 期:Object(Struct/Array/Map)属性递归建表;一个子项都没设过 → Nil(不动脚本自己的那张表)。
		//   * Struct:子字段按名字写(未设的子字段跳过);
		//   * Array:Lua 数组 1..n(第一个未设元素之后的元素不写 —— 数组不能有洞);
		//   * Map:键按 KeyKind 写(字符串键 / 数字键)。
		ScriptValue BuildPropertyScriptValue(const ScriptProperty& property)
		{
			if (!s_Vm)
				return ScriptValue::Nil();
			if (property.Type != Schema::Kind::Object)
			{
				if (std::holds_alternative<std::monostate>(property.Value))
					return ScriptValue::Nil();   // 未设 → 跳过(不写 nil,不清脚本字段)
				return ToScriptValueInternal(property.Value);
			}
			ScriptTableRef table = s_Vm->CreateTable();
			if (!table.IsValid())
				return ScriptValue::Nil();
			bool wrote = false;
			if (property.Collection == ScriptPropertyCollection::Map)
			{
				for (const ScriptProperty& child : property.Children)
				{
					const ScriptValue childValue = BuildPropertyScriptValue(child);
					if (childValue.IsNil())
						continue;   // 未设的键跳过(保留脚本自己的条目)
					SetScriptTableKey(table, child.Name, property.KeyKind, childValue);
					wrote = true;
				}
			}
			else if (property.Collection == ScriptPropertyCollection::Array)
			{
				std::size_t index = 0;
				for (const ScriptProperty& child : property.Children)
				{
					const ScriptValue childValue = BuildPropertyScriptValue(child);
					if (childValue.IsNil())
						break;   // 数组不能有洞:第一个未设元素之后不再写
					if (!table.SetArrayElement(++index, childValue))
						throw std::logic_error("Cannot assign script array element " + std::to_string(index));
					wrote = true;
				}
			}
			else
			{
				for (const ScriptProperty& child : property.Children)
				{
					const ScriptValue childValue = BuildPropertyScriptValue(child);
					if (childValue.IsNil())
						continue;   // 未设的子字段跳过(保留脚本自己的默认值)
					if (!table.SetField(child.Name.c_str(), childValue))
						throw std::logic_error("Cannot assign script table field '" + child.Name + "'");
					wrote = true;
				}
			}
			return wrote ? table.ToValue() : ScriptValue::Nil();
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

		void ShutdownInternal()
		{
			// W4:VM 关闭前先丢弃事件/计时器订阅(引用在 VM 关闭后一律失效)。
			ResetScriptEventSubscriptions();
			// T4:插件脚本函数账本保留,但"已绑定"状态必须清掉(下一次 Init 重新绑定)。
			PluginScriptLibrary::OnVmShutdown();
			s_LookupField.Release();
			s_CollectFieldNames.Release();
			s_CollectEntries.Release();
			s_SetIndex.Release();
			s_Bindings.reset();
			if (s_Vm)
				s_Vm->Shutdown();
			s_Vm.reset();
			s_ActiveScene = nullptr;
			s_SystemScripts.clear();
			s_LoadingSystemScript.clear();
			s_SystemWatch.reset();
			s_OwnerThread = {};
		}
	}

	bool ScriptEngine::IsInitialized() { return s_Vm != nullptr; }

	void ScriptEngine::AssertOwnerThread()
	{
		if (!s_Vm) throw std::logic_error("ScriptEngine is not initialized");
		if (s_OwnerThread != std::this_thread::get_id()) throw std::logic_error("Lua access must run on the ScriptEngine owner thread");
	}

	void ScriptEngine::SetActiveScene(Scene* scene)
	{
		s_ActiveScene = scene;
	}

	Scene* ScriptEngine::GetActiveScene()
	{
		return s_ActiveScene;
	}

	// 系统脚本目录 → 逻辑路径前缀(<内容根>/scripts/systems → "scripts/systems/")。
	// 逻辑路径是 VFS/磁盘的统一寻址口径(ResolveScriptSource / 热重载指纹都用它)。
	static std::string SystemScriptPrefix(const std::filesystem::path& systemsDir)
	{
		const std::string leaf = systemsDir.filename().string();
		const std::string parent = systemsDir.parent_path().filename().string();
		if (parent.empty())
			return leaf + "/";
		return parent + "/" + leaf + "/";
	}

	static bool IsSystemScriptFile(const std::filesystem::directory_entry& entry)
	{
		if (!entry.is_regular_file())
			return false;
		const std::string ext = entry.path().extension().string();
		return ext == ".luau" || ext == ".lua";
	}

	void ScriptEngine::NoteScriptSystem(const std::string& systemName)
	{
		if (s_LoadingSystemScript.empty() || systemName.empty())
			return;
		SystemScriptRecord* record = FindSystemScriptRecord(s_LoadingSystemScript);
		if (!record)
		{
			s_SystemScripts.push_back(SystemScriptRecord{ s_LoadingSystemScript, {} });
			record = &s_SystemScripts.back();
		}
		record->SystemNames.push_back(systemName);
	}

	std::size_t ScriptEngine::ReloadSystemScript(Scene& scene, const std::string& logicalPath)
	{
		if (!IsInitialized() || logicalPath.empty())
			return 0;

		SetActiveScene(&scene);

		// 1) 撤销这份系统脚本上次注册的系统(先拷一份名字,避免边遍历边改容器)。
		if (SystemScriptRecord* existing = FindSystemScriptRecord(logicalPath))
		{
			const std::vector<std::string> names = existing->SystemNames;
			existing->SystemNames.clear();
			for (const std::string& systemName : names)
				scene.UnregisterFrameSystem(systemName);
		}

		// 2) 读源(VFS 优先、磁盘回退,与其余脚本读取同一语义)。
		std::string source;
		std::string readError;
		if (!ResolveScriptSource(logicalPath, source, &readError) || source.empty())
		{
			if (Log::GetCoreLogger())
				WLD_CORE_ERROR("[Luau System Loader] cannot read '{}': {}", logicalPath, readError);
			return 0;
		}

		// 3) 整份重跑;期间的 ecs:AddSystem 经 s_LoadingSystemScript 归属到本文件。
		s_LoadingSystemScript = logicalPath;
		std::string error;
		const bool ok = s_Vm->RunString(source, logicalPath.c_str(), &error);
		s_LoadingSystemScript.clear();

		const SystemScriptRecord* record = FindSystemScriptRecord(logicalPath);
		const std::size_t systems = record ? record->SystemNames.size() : 0;
		if (!ok)
		{
			if (Log::GetCoreLogger())
				WLD_CORE_ERROR("[Luau System Loader] '{}' failed: {}", logicalPath, error);
			return 0;
		}
		if (Log::GetCoreLogger())
			WLD_CORE_INFO("[Luau System Loader] '{}' -> {} system(s)", logicalPath, systems);
		return systems;
	}

	std::size_t ScriptEngine::UnloadSystemScripts(Scene& scene)
	{
		std::size_t removed = 0;
		for (const SystemScriptRecord& record : s_SystemScripts)
			for (const std::string& systemName : record.SystemNames)
				if (scene.UnregisterFrameSystem(systemName))
					++removed;
		s_SystemScripts.clear();
		s_LoadingSystemScript.clear();
		return removed;
	}

	std::size_t ScriptEngine::LoadSystemScripts(Scene& scene, const std::filesystem::path& systemsDir)
	{
		if (!IsInitialized() || !std::filesystem::exists(systemsDir) || !std::filesystem::is_directory(systemsDir))
			return 0;

		SetActiveScene(&scene);
		const std::string prefix = SystemScriptPrefix(systemsDir);
		std::size_t systems = 0;
		for (const auto& entry : std::filesystem::directory_iterator(systemsDir))
		{
			if (!IsSystemScriptFile(entry))
				continue;
			systems += ReloadSystemScript(scene, prefix + entry.path().filename().string());
		}
		return systems;
	}

	std::size_t ScriptEngine::LoadSystemScripts(Scene& scene)
	{
		return LoadSystemScripts(scene, World::Paths::AssetRoot() / "scripts/systems");
	}

	std::size_t ScriptEngine::PollSystemScriptReload(Scene& scene, double deltaSeconds)
	{
		if (!IsInitialized())
			return 0;
		const std::filesystem::path dir = World::Paths::AssetRoot() / "scripts/systems";
		if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
			return 0;

		if (!s_SystemWatch)
			s_SystemWatch = std::make_unique<ScriptFileWatch>();
		const std::string prefix = SystemScriptPrefix(dir);
		for (const auto& entry : std::filesystem::directory_iterator(dir))
		{
			if (!IsSystemScriptFile(entry))
				continue;
			const std::string logical = prefix + entry.path().filename().string();
			if (!s_SystemWatch->IsWatched(logical))
				s_SystemWatch->Watch(logical);   // 新文件:建基线,本帧不算变化
		}

		std::size_t processed = 0;
		for (const std::string& changed : s_SystemWatch->Poll(deltaSeconds))
		{
			ReloadSystemScript(scene, changed);
			++processed;
		}
		return processed;
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

			// SCRIPT-V11:脚本的 `print` 由 `LuauVm::Init` 装好(`[script] ` + INFO,见 LuauVm.cpp 的
			// scriptPrint),这里**不得再覆盖** —— 旧覆盖把它换成 trace 级的 `[Lua] …`,
			// 默认 log_level=info 下用户在编辑器/Scripts 面板完全看不到脚本输出。
			// (沙箱纪律不变:所有宿主注入仍必须发生在**编译脚本之前**。)

			s_LookupField = CompileHelper(kFieldLookupSource, "WorldEngine.FieldLookup", &error);
			s_CollectFieldNames = CompileHelper(kFieldCollectorSource, "WorldEngine.FieldNames", &error);
			s_CollectEntries = CompileHelper(kEntryCollectorSource, "WorldEngine.FieldEntries", &error);
			s_SetIndex = CompileHelper(kSetIndexSource, "WorldEngine.SetIndex", &error);
			if (!s_LookupField.IsValid() || !s_CollectFieldNames.IsValid() ||
				!s_CollectEntries.IsValid() || !s_SetIndex.IsValid())
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
			// Pure ECS: 注册全局 ecs 与 world 绑定表 (Script/BindECS.h)
			if (!RegisterEcsBindings(*s_Bindings, &error))
				throw std::runtime_error("[Lua] failed to register the ECS bindings: " + error);
			// T4:插件脚本函数库(插件可能在本 VM 之前注册 —— 账本在 Script/PluginScriptLibrary.h,
			// 这里统一绑定;运行时绑定与存根渲染消费同一份描述)。
			if (!PluginScriptLibrary::BindAll(*s_Bindings, &error))
				throw std::runtime_error("[Lua] failed to bind the plugin script functions: " + error);
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
	// → 报"包内不可编辑";否则要求 <当前内容根>/<path> 是常规文件,成功时返回其绝对路径。
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
			World::Paths::AssetRoot() / std::filesystem::path(normalized);
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
		const std::filesystem::path path(World::Paths::AssetRoot() / "scripts/intermediate/WorldEngineAPI.luau");
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
		const std::filesystem::path path(World::Paths::AssetRoot() / "scripts/intermediate/WorldEngineAPI.luau");
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
		// Pure ECS:`ecs` / `world` 表与 events/timers 同属"全局只读表",走同一条存根渲染链路
		// (描述表在 BindECS.cpp,注册循环与存根共用同一份)。
		std::size_t ecsCount = 0;
		const ScriptServiceBinding* ecsTables = ScriptEcsBindings(&ecsCount);
		for (std::size_t index = 0; index < ecsCount; ++index)
			serviceList.push_back(&ecsTables[index]);
		// T4:插件脚本函数库块追加在既有全部块之后(顺序 = 命名空间升序、组内 (插件 id, 函数名);
		// 零插件 = 空列表 ⇒ 存根与入库夹具逐字节一致)。
		if (!LuaStubGenerator::Generate(path, LuaReflectionRegistry::GetTable(), components, serviceList,
			uiList, PluginScriptLibrary::StubTables(), error))
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





}
