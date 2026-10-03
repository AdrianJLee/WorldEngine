#include "World/Script/LuauCompletion.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <unordered_set>

namespace World
{
	namespace
	{
		constexpr std::size_t kNone = std::string_view::npos;

		bool IsIdentStart(char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		}

		bool IsIdentPart(char c)
		{
			return IsIdentStart(c) || (c >= '0' && c <= '9');
		}

		bool IsBlank(char c)
		{
			// '\n' 也算空白:整段源码的"是否为空"判定要能穿过换行(逐行扫描前每行已被切走)。
			return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
		}

		std::string_view Trim(std::string_view text)
		{
			std::size_t begin = 0;
			while (begin < text.size() && IsBlank(text[begin]))
				++begin;
			std::size_t end = text.size();
			while (end > begin && IsBlank(text[end - 1]))
				--end;
			return text.substr(begin, end - begin);
		}

		bool StartsWith(std::string_view text, std::string_view prefix)
		{
			return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
		}

		// 以单词开头且后面确实是分隔(空白/EOF/'('),避免把 "localx" 当成 "local"。
		bool StartsWithWord(std::string_view text, std::string_view word)
		{
			if (!StartsWith(text, word))
				return false;
			return text.size() == word.size() || IsBlank(text[word.size()]) || text[word.size()] == '(';
		}

		std::string_view FirstWord(std::string_view text)
		{
			std::size_t end = 0;
			while (end < text.size() && !IsBlank(text[end]))
				++end;
			return text.substr(0, end);
		}

		char LowerAscii(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}

		bool EqualsIgnoreCase(std::string_view left, std::string_view right)
		{
			if (left.size() != right.size())
				return false;
			for (std::size_t i = 0; i < left.size(); ++i)
				if (LowerAscii(left[i]) != LowerAscii(right[i]))
					return false;
			return true;
		}

		bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
		{
			if (text.size() < prefix.size())
				return false;
			for (std::size_t i = 0; i < prefix.size(); ++i)
				if (LowerAscii(text[i]) != LowerAscii(prefix[i]))
					return false;
			return true;
		}

		bool ContainsIgnoreCase(std::string_view text, std::string_view needle)
		{
			if (needle.empty())
				return true;
			if (needle.size() > text.size())
				return false;
			for (std::size_t start = 0; start + needle.size() <= text.size(); ++start)
			{
				std::size_t i = 0;
				while (i < needle.size() && LowerAscii(text[start + i]) == LowerAscii(needle[i]))
					++i;
				if (i == needle.size())
					return true;
			}
			return false;
		}

		int CompareIgnoreCase(std::string_view left, std::string_view right)
		{
			const std::size_t shared = std::min(left.size(), right.size());
			for (std::size_t i = 0; i < shared; ++i)
			{
				const char a = LowerAscii(left[i]);
				const char b = LowerAscii(right[i]);
				if (a != b)
					return (a < b) ? -1 : 1;
			}
			if (left.size() == right.size())
				return 0;
			return (left.size() < right.size()) ? -1 : 1;
		}

		std::string LowerCopy(std::string_view text)
		{
			std::string result(text);
			for (char& c : result)
				c = LowerAscii(c);
			return result;
		}

		// 显示顺序:Field→Method→Global→Class→Keyword(Class 是"可当接收者的名字"档,
		// 紧跟在 Global 后面,关键字固定在最后)。
		int KindRank(LuauCompletionItem::KindType kind)
		{
			using Kind = LuauCompletionItem::KindType;
			switch (kind)
			{
				case Kind::Field: return 0;
				case Kind::Method: return 1;
				case Kind::Global: return 2;
				case Kind::Class: return 3;
				case Kind::Keyword: return 4;
			}
			return 5;
		}

		// Luau 关键字与字面量(含 Luau 追加的 continue/export/type/typeof)。
		constexpr std::string_view kKeywords[] = {
			"and", "break", "continue", "do", "else", "elseif", "end", "export", "false",
			"for", "function", "if", "in", "local", "nil", "not", "or", "repeat", "return",
			"then", "true", "type", "typeof", "until", "while",
		};

		struct BuiltinName
		{
			std::string_view Name;
			const char* Doc;
		};

		// 沙箱内置:LuauVm.cpp 的 kAllowedLibraries 里的库名 + base 库里没有被
		// kForbiddenGlobals 移除的函数(os/debug/require/rawget/... 一律不给补全)。
		constexpr BuiltinName kBuiltinLibraries[] = {
			{ "bit32", "Luau sandbox library" },
			{ "coroutine", "Luau sandbox library" },
			{ "math", "Luau sandbox library" },
			{ "string", "Luau sandbox library" },
			{ "table", "Luau sandbox library" },
			{ "utf8", "Luau sandbox library" },
		};
		constexpr BuiltinName kBuiltinFunctions[] = {
			{ "assert", "Luau base function (sandbox)" },
			{ "error", "Luau base function (sandbox)" },
			{ "gcinfo", "Luau base function (sandbox)" },
			{ "getmetatable", "Luau base function (sandbox)" },
			{ "ipairs", "Luau base function (sandbox)" },
			{ "next", "Luau base function (sandbox)" },
			{ "pairs", "Luau base function (sandbox)" },
			{ "pcall", "Luau base function (sandbox)" },
			{ "print", "Luau base function (sandbox)" },
			{ "select", "Luau base function (sandbox)" },
			{ "setmetatable", "Luau base function (sandbox)" },
			{ "tonumber", "Luau base function (sandbox)" },
			{ "tostring", "Luau base function (sandbox)" },
			{ "type", "Luau base function (sandbox)" },
			{ "typeof", "Luau base function (sandbox)" },
			{ "xpcall", "Luau base function (sandbox)" },
		};

		struct FieldSpec
		{
			std::string_view Name;
			std::string_view Type;
			std::string_view Desc;
		};

		// `fun(self: X, dt: number)` → {"dt"}(跳过 self/.../空名;支持嵌套括号里的逗号)。
		std::vector<std::string> ExtractFunParams(std::string_view type)
		{
			std::vector<std::string> params;
			const std::size_t open = type.find('(');
			if (open == std::string_view::npos)
				return params;
			std::size_t depth = 0;
			std::size_t end = open;
			for (; end < type.size(); ++end)
			{
				if (type[end] == '(')
					++depth;
				else if (type[end] == ')')
				{
					--depth;
					if (depth == 0)
						break;
				}
			}
			const std::string_view inner = type.substr(open + 1,
				std::min(end, type.size()) - (open + 1));
			std::size_t start = 0;
			depth = 0;
			for (std::size_t i = 0; i <= inner.size(); ++i)
			{
				const bool atEnd = i == inner.size();
				const char c = atEnd ? ',' : inner[i];
				if (!atEnd && (c == '(' || c == '{'))
					++depth;
				else if (!atEnd && (c == ')' || c == '}'))
					--depth;
				if (c != ',' || depth != 0)
					continue;
				std::string_view part = Trim(inner.substr(start, i - start));
				start = i + 1;
				const std::size_t colon = part.find(':');
				if (colon != std::string_view::npos)
					part = Trim(part.substr(0, colon));
				if (part.empty() || part == "self" || part == "...")
					continue;
				params.emplace_back(part);
			}
			return params;
		}

		// `---@field <name> <type> <desc>`:name 允许可选后缀 '?';type 形如 fun(self: X)
		// 时按平衡括号取整(里面的空格不能把类型截成两段),其余取下一个空白词。
		FieldSpec ParseFieldSpec(std::string_view rest)
		{
			FieldSpec spec;
			const std::string_view rawName = FirstWord(rest);
			spec.Name = rawName;
			if (!spec.Name.empty() && spec.Name.back() == '?')
				spec.Name.remove_suffix(1);

			const std::string_view remainder = Trim(rest.substr(rawName.size()));
			if (StartsWith(remainder, "fun("))
			{
				std::size_t depth = 0;
				std::size_t i = 0;
				for (; i < remainder.size(); ++i)
				{
					if (remainder[i] == '(')
						++depth;
					else if (remainder[i] == ')')
					{
						--depth;
						if (depth == 0)
						{
							++i;
							break;
						}
					}
				}
				spec.Type = remainder.substr(0, i);
				spec.Desc = Trim(remainder.substr(i));
			}
			else
			{
				spec.Type = FirstWord(remainder);
				spec.Desc = Trim(remainder.substr(spec.Type.size()));
			}
			return spec;
		}

		// ---- V9:注解的**类型位**(用户反馈:「注释中填类型时没有提示」)----
		//
		// 认这四种位(其余位置不是类型位,不要给人塞类型候选):
		//   `---@type <前缀>`            —— 标签后第 1 个 token
		//   `---@field <名字> <前缀>`     —— 第 2 个 token
		//   `---@param <名字> <前缀>`     —— 第 2 个 token
		//   `---@class <名字> : <前缀>`   —— 继承位(冒号之后的那个 token;`X:` 连写也算)
		// linePrefix = 光标前的整行片段;outPrefix = 正在输入的标识符前缀(可为空)。
		bool IsAnnotationTypePosition(std::string_view linePrefix, std::string_view* outPrefix)
		{
			const std::size_t at = linePrefix.rfind("---@");
			if (at == std::string_view::npos)
				return false;
			const std::string_view body = linePrefix.substr(at + 4);
			// 光标前的 partial token:标识符前缀(空 = 刚打完空格,准备输入类型)。
			std::size_t tokenStart = body.size();
			while (tokenStart > 0 && IsIdentPart(body[tokenStart - 1]))
				--tokenStart;
			if (tokenStart < body.size() && !IsIdentStart(body[tokenStart]))
				return false;   // 光标前既不是标识符也不是空白/行首
			const std::string_view partial = body.substr(tokenStart);
			// 之前的部分按空白切 token(注解语法里 token 之间只有空白)。
			std::vector<std::string_view> tokens;
			{
				const std::string_view head = body.substr(0, tokenStart);
				std::size_t cursor = 0;
				while (cursor < head.size())
				{
					while (cursor < head.size() && IsBlank(head[cursor]))
						++cursor;
					const std::size_t start = cursor;
					while (cursor < head.size() && !IsBlank(head[cursor]))
						++cursor;
					if (cursor > start)
						tokens.push_back(head.substr(start, cursor - start));
				}
			}
			if (tokens.empty())
				return false;
			const std::string_view tag = tokens.front();
			if (tag == "class")
			{
				// 继承位 = 最后一个 token 以 ':' 结尾(`---@class X : ` 或 `---@class X: `)。
				const std::string_view last = tokens.back();
				if (tokens.size() >= 2 && !last.empty() && last.back() == ':')
				{
					*outPrefix = partial;
					return true;
				}
				return false;
			}
			const std::size_t expectedTokens = (tag == "type") ? 1u : 2u;
			if (tag != "type" && tag != "field" && tag != "param")
				return false;
			if (tokens.size() != expectedTokens)
				return false;   // 说明文本(第 3 个 token 起)/ 别的 tag 位置不给类型候选
			*outPrefix = partial;
			return true;
		}

		// V9:注解类型位的"基础类型"档(固定顺序)。
		struct AnnotationBaseType
		{
			const char* Name;
			const char* Doc;
		};

		// D1(2026-09-26):常用基础类型排在候选最前(首屏预算约 10-12 行)。
		constexpr AnnotationBaseType kAnnotationCommonBaseTypes[] = {
			{ "number", "数值(Luau 的 number)" },
			{ "string", "字符串" },
			{ "boolean", "布尔值(true / false)" },
			{ "integer", "整数(64 位整数值)" },
			{ "table", "表(可用 { [K]: V } 细化)" },
			{ "any", "任意类型(不检查成员)" },
		};

		// 其余 Luau 基础类型:顺序保持修复前不变;只是挪到引擎类型之后(打前缀仍能命中)。
		constexpr AnnotationBaseType kAnnotationOtherBaseTypes[] = {
			{ "buffer", "二进制缓冲区" },
			{ "function", "函数(可用 fun(...): ... 细化签名)" },
			{ "never", "永不返回(如 error 的返回类型)" },
			{ "nil", "空值" },
			{ "thread", "协程线程" },
			{ "unknown", "未知类型(使用前需要收窄)" },
			{ "vector", "向量(如 vec3.new 的返回)" },
		};

		// D1:引擎类型档 —— 顺序即方案 D1 的固定顺序;**是否出现由索引里是否真有同名
		// `---@class`(存根或当前文件)决定**,不凭名字硬造候选。Doc 优先取存根注释块;
		// 存根(`LuaStubGenerator`)对 vec2/vec3/vec4/mat3/mat4/Entity 没有散文说明,这里给
		// 一句话兜底(存根将来补了说明就以存根为准,不另抄第二份)。
		struct AnnotationEngineType
		{
			const char* Name;
			const char* FallbackDoc;
		};

		constexpr AnnotationEngineType kAnnotationEngineTypes[] = {
			{ "vec2", "引擎向量类型(Lua 侧是 userdata;vec2.new 构造,含 x/y 坐标)" },
			{ "vec3", "引擎向量类型(Lua 侧是 userdata;vec3.new 构造,含 x/y/z 坐标)" },
			{ "vec4", "引擎向量类型(Lua 侧是 userdata;vec4.new 构造,含 x/y/z/w 坐标)" },
			{ "mat3", "引擎矩阵类型(Lua 侧是 userdata;mat3.new 构造)" },
			{ "mat4", "引擎矩阵类型(Lua 侧是 userdata;mat4.new 构造)" },
			{ "Entity", "实体句柄(Lua 侧是 userdata;Entity 方法表的接收者)" },
			{ "WorldScript", "脚本实例(注解形态的类;回调里的 self 就是它)" },
		};

		// ---- D3(2026-09-27):文件内局部变量/表字段的类型推断用的文本工具 ----

		std::size_t SkipBlanksAt(std::string_view text, std::size_t pos)
		{
			while (pos < text.size() && IsBlank(text[pos]))
				++pos;
			return pos;
		}

		// 注释与字符串**内容**替换成空格(引号与换行保留):推断只看代码结构,
		// 不被注释里的 `local x = 1`、字符串里的 `{ a = 1 }` 干扰。
		std::string MaskCommentsAndStrings(std::string_view text)
		{
			std::string masked(text);
			std::size_t index = 0;
			while (index < masked.size())
			{
				const char character = masked[index];
				if (character == '-' && index + 1 < masked.size() && masked[index + 1] == '-')
				{
					// 长注释 --[[ … ]] / --[=[ … ]=](简写:-- 后紧跟 '[' 就算)。
					std::size_t level = 0;
					std::size_t content = index + 2;
					bool longComment = false;
					if (content < masked.size() && masked[content] == '[')
					{
						std::size_t probe = content + 1;
						while (probe < masked.size() && masked[probe] == '=')
						{
							++level;
							++probe;
						}
						if (probe < masked.size() && masked[probe] == '[')
						{
							longComment = true;
							content = probe + 1;
						}
					}
					std::size_t stop = masked.size();
					if (longComment)
					{
						const std::string closing = "]" + std::string(level, '=') + "]";
						const std::size_t found = masked.find(closing, content);
						stop = found == std::string::npos ? masked.size() : found + closing.size();
					}
					else
					{
						const std::size_t found = masked.find('\n', index);
						stop = found == std::string::npos ? masked.size() : found;
					}
					for (std::size_t cursor = index; cursor < stop; ++cursor)
						if (masked[cursor] != '\n')
							masked[cursor] = ' ';
					index = stop;
					continue;
				}
				if (character == '"' || character == '\'')
				{
					std::size_t cursor = index + 1;
					while (cursor < masked.size() && masked[cursor] != character)
					{
						if (masked[cursor] == '\\' && cursor + 1 < masked.size())
						{
							if (masked[cursor + 1] != '\n') masked[cursor + 1] = ' ';
							cursor += 2;
							continue;
						}
						if (masked[cursor] != '\n') masked[cursor] = ' ';
						++cursor;
					}
					index = cursor < masked.size() ? cursor + 1 : cursor;
					continue;
				}
				if (character == '[' && index + 1 < masked.size() && masked[index + 1] == '[')
				{
					const std::size_t found = masked.find("]]", index + 2);
					const std::size_t stop = found == std::string::npos ? masked.size() : found;
					for (std::size_t cursor = index + 2; cursor < stop; ++cursor)
						if (masked[cursor] != '\n')
							masked[cursor] = ' ';
					index = found == std::string::npos ? masked.size() : stop + 2;
					continue;
				}
				++index;
			}
			return masked;
		}

		// 一个表达式/值的结尾(顶层 ',' / ';' / 换行 / 收尾括号)。用于跳过已经推断过的值。
		std::size_t SkipValueExpression(std::string_view masked, std::size_t pos)
		{
			int depth = 0;
			while (pos < masked.size())
			{
				const char character = masked[pos];
				if (character == '{' || character == '(' || character == '[')
				{
					++depth;
					++pos;
					continue;
				}
				if (character == '}' || character == ')' || character == ']')
				{
					if (depth == 0)
						return pos;
					--depth;
					++pos;
					continue;
				}
				if (depth == 0 && (character == ',' || character == ';' || character == '\n'))
					return pos;
				if (character == '"' || character == '\'')
				{
					++pos;
					while (pos < masked.size() && masked[pos] != character)
						++pos;
					if (pos < masked.size())
						++pos;
					continue;
				}
				++pos;
			}
			return pos;
		}

		std::size_t NextLineStart(std::string_view masked, std::size_t pos)
		{
			const std::size_t found = masked.find('\n', pos);
			return found == std::string_view::npos ? masked.size() : found + 1;
		}

		// E2(2026-09-27 用户口径「局部变量从函数返回值/成员表达式赋值也要有类型提示」):
		// `---@return` 可能是联合(`userdata|nil`)——取第一个不是 nil 的备选;全是 nil/空 → 空。
		std::string FirstReturnType(std::string_view type)
		{
			std::size_t start = 0;
			while (start <= type.size())
			{
				const std::size_t bar = type.find('|', start);
				const std::string_view part = Trim(type.substr(start,
					bar == std::string_view::npos ? std::string_view::npos : bar - start));
				if (!part.empty() && part != "nil")
					return std::string(part);
				if (bar == std::string_view::npos)
					break;
				start = bar + 1;
			}
			return std::string();
		}
	}

	void LuauCompletionIndex::Clear()
	{
		m_Items.clear();
		m_ItemByName.clear();
		m_StubClasses.clear();
		m_FileItems.clear();
		m_FileClasses.clear();
		m_AnnotationFields.clear();
		m_Inferred.clear();
	}

	bool LuauCompletionIndex::LoadStub(std::string_view text, std::string* error)
	{
		if (error)
			error->clear();

		if (Trim(text).empty())
		{
			if (error)
				*error = "stub source is empty";
			return false;   // 失败不改动索引:调用方可以继续用旧的符号表
		}

		m_Items.clear();
		m_ItemByName.clear();
		m_StubClasses.clear();

		// `---@` 标签候选(只在注解上下文出现,不混进普通代码补全)。
		struct TagSpec { const char* Name; const char* Doc; };
		static const TagSpec kAnnotationTags[] = {
			{ "class", "声明一个类(---@class Name[: Base])。" },
			{ "field", "给当前类声明字段(---@field name[?] type 说明)。" },
			{ "param", "说明函数参数(---@param name type 说明)。" },
			{ "return", "说明返回值类型(---@return type)。" },
			{ "type", "给变量/局部量标注类型(---@type Type)。" },
			{ "overload", "补充一个函数重载签名(---@overload fun(...))。" },
			{ "meta", "把文件标记为仅供类型检查的存根(---@meta)。" },
			{ "diagnostic", "调整本文件的诊断开关(---@diagnostic disable: ...)。" },
		};
		m_Tags.clear();
		for (const TagSpec& spec : kAnnotationTags)
		{
			LuauCompletionItem tag;
			tag.Name = spec.Name;
			tag.Doc = spec.Doc;
			tag.Kind = LuauCompletionItem::KindType::Keyword;
			m_Tags.push_back(std::move(tag));
		}

		ParseStubText(text);

		auto addBuiltin = [&](std::string_view name, std::string_view doc,
			LuauCompletionItem::KindType kind)
		{
			if (m_ItemByName.find(std::string(name)) != m_ItemByName.end())
				return;   // 存根自己声明过的名字不重复加
			LuauCompletionItem item;
			item.Name.assign(name);
			item.Doc.assign(doc);
			item.Kind = kind;
			m_ItemByName.emplace(item.Name, m_Items.size());
			m_Items.push_back(std::move(item));
		};
		for (const std::string_view keyword : kKeywords)
			addBuiltin(keyword, std::string_view(), LuauCompletionItem::KindType::Keyword);
		for (const BuiltinName& library : kBuiltinLibraries)
			addBuiltin(library.Name, library.Doc, LuauCompletionItem::KindType::Global);
		for (const BuiltinName& function : kBuiltinFunctions)
			addBuiltin(function.Name, function.Doc, LuauCompletionItem::KindType::Global);
		return true;
	}

	bool LuauCompletionIndex::LoadStubFile(const std::string& path, std::string* error)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			if (error)
				*error = "cannot open stub file: " + path;
			return false;
		}
		std::string text;
		text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		if (!file.good() && !file.eof())
		{
			if (error)
				*error = "cannot read stub file: " + path;
			return false;
		}
		return LoadStub(text, error);
	}

	void LuauCompletionIndex::ParseStubText(std::string_view text)
	{
		bool inCommentBlock = false;
		bool docTaken = false;
		std::string blockDoc;
		std::string blockReturn;
		std::vector<std::string> blockParams;   // W9.8:---@param 顺序(去 self)
		std::vector<std::string> blockParamTypes;
		std::size_t currentClass = kNone;

		auto resetBlock = [&]()
		{
			inCommentBlock = false;
			docTaken = false;
			blockDoc.clear();
			blockReturn.clear();
			blockParams.clear();
			blockParamTypes.clear();
		};

		auto ensureClass = [&](std::string_view name, std::string_view base) -> std::size_t
		{
			for (std::size_t i = 0; i < m_StubClasses.size(); ++i)
			{
				if (m_StubClasses[i].Name == name)
				{
					if (m_StubClasses[i].Base.empty() && !base.empty())
						m_StubClasses[i].Base.assign(base);
					return i;
				}
			}
			ClassInfo info;
			info.Name.assign(name);
			info.Base.assign(base);
			m_StubClasses.push_back(std::move(info));
			return m_StubClasses.size() - 1;
		};

		auto addItem = [&](std::string_view name, std::string_view type, std::string_view doc,
			LuauCompletionItem::KindType kind)
		{
			const auto found = m_ItemByName.find(std::string(name));
			if (found != m_ItemByName.end())
			{
				LuauCompletionItem& existing = m_Items[found->second];
				// `---@class X` + `X = {}`(同名):升级成运行时全局项,文档/类型保留先到的。
				if (existing.Kind == LuauCompletionItem::KindType::Class
					&& kind == LuauCompletionItem::KindType::Global)
				{
					existing.Kind = LuauCompletionItem::KindType::Global;
					if (existing.Type.empty())
						existing.Type.assign(type);
					if (existing.Doc.empty())
						existing.Doc.assign(doc);
				}
				return;
			}
			LuauCompletionItem item;
			item.Name.assign(name);
			item.Type.assign(type);
			item.Doc.assign(doc);
			item.Kind = kind;
			m_ItemByName.emplace(item.Name, m_Items.size());
			m_Items.push_back(std::move(item));
		};

		auto addMember = [&](std::size_t classIndex, std::string_view name, std::string_view type,
			std::string_view doc, LuauCompletionItem::KindType kind,
			const std::vector<std::string>& params = {},
			const std::vector<std::string>& paramTypes = {})
		{
			ClassInfo& info = m_StubClasses[classIndex];
			for (const LuauCompletionItem& member : info.Members)
				if (member.Name == name)
					return;   // 同名成员保留首个
			LuauCompletionItem item;
			item.Name.assign(name);
			item.Type.assign(type);
			item.Doc.assign(doc);
			item.Params = params;
			item.ParamTypes = paramTypes;
			item.Kind = kind;
			info.Members.push_back(std::move(item));
		};

		std::size_t lineStart = 0;
		while (lineStart <= text.size())
		{
			const std::size_t lineEnd = text.find('\n', lineStart);
			const bool lastLine = lineEnd == kNone;
			const std::string_view line = Trim(text.substr(lineStart,
				(lastLine ? text.size() : lineEnd) - lineStart));
			lineStart = lastLine ? text.size() + 1 : lineEnd + 1;

			if (StartsWith(line, "---"))
			{
				if (!inCommentBlock)
				{
					inCommentBlock = true;
					docTaken = false;
					blockDoc.clear();
					blockReturn.clear();
				}
				const std::string_view body = line.substr(3);
				if (StartsWith(body, "@"))
				{
					const std::string_view tag = FirstWord(body.substr(1));
					const std::string_view rest = Trim(body.substr(1 + tag.size()));
					if (tag == "class")
					{
						const std::string_view name = FirstWord(rest);
						const std::string_view tail = Trim(rest.substr(name.size()));
						std::string_view base;
						if (!tail.empty() && tail.front() == ':')
							base = FirstWord(Trim(tail.substr(1)));
						if (!name.empty() && IsIdentStart(name.front()))
						{
							currentClass = ensureClass(name, base);
							addItem(name, std::string_view(), blockDoc,
								LuauCompletionItem::KindType::Class);
						}
					}
					else if (tag == "field")
					{
						if (currentClass != kNone)
						{
							const FieldSpec spec = ParseFieldSpec(rest);
							if (!spec.Name.empty() && IsIdentStart(spec.Name.front()))
								addMember(currentClass, spec.Name, spec.Type, spec.Desc,
									LuauCompletionItem::KindType::Field, ExtractFunParams(spec.Type));
						}
					}
					else if (tag == "return")
					{
						if (blockReturn.empty())
						{
							const std::string_view type = FirstWord(rest);
							if (!type.empty())
								blockReturn.assign(type);
						}
					}
					else if (tag == "param")
					{
						// W9.8:参数名进补全项,接受时插 `name(p1, p2)` 并选中第一个参数。
						const std::string_view name = FirstWord(rest);
						const std::string_view afterName = Trim(rest.substr(name.size()));
						std::string_view type = FirstWord(afterName);
						if (!type.empty() && type.back() == '?')
							type.remove_suffix(1);
						if (!name.empty() && name != "self" && name != "...")
						{
							blockParams.emplace_back(name);
							blockParamTypes.emplace_back(type);
						}
					}
					// @overload/@operator/@meta 与未知 tag:忽略,不打断注释块
				}
				else
				{
					const std::string_view prose = Trim(body);
					if (!docTaken && !prose.empty())
					{
						blockDoc.assign(prose);
						docTaken = true;
					}
				}
				continue;
			}

			if (!line.empty() && !StartsWith(line, "--"))
			{
				if (StartsWithWord(line, "function"))
				{
					const std::string_view rest = Trim(line.substr(8));
					std::size_t ownerEnd = 0;
					while (ownerEnd < rest.size() && IsIdentPart(rest[ownerEnd]))
						++ownerEnd;
					const std::string_view owner = rest.substr(0, ownerEnd);
					if (!owner.empty() && IsIdentStart(owner.front()) && ownerEnd < rest.size()
						&& (rest[ownerEnd] == ':' || rest[ownerEnd] == '.'))
					{
						std::size_t methodEnd = ownerEnd + 1;
						while (methodEnd < rest.size() && IsIdentPart(rest[methodEnd]))
							++methodEnd;
						const std::string_view method =
							rest.substr(ownerEnd + 1, methodEnd - (ownerEnd + 1));
						if (!method.empty())
						{
							addMember(ensureClass(owner, std::string_view()), method, blockReturn,
								blockDoc, LuauCompletionItem::KindType::Method, blockParams, blockParamTypes);
						}
					}
				}
				else
				{
					std::size_t nameEnd = 0;
					while (nameEnd < line.size() && IsIdentPart(line[nameEnd]))
						++nameEnd;
					const std::string_view name = line.substr(0, nameEnd);
					const std::string_view tail = Trim(line.substr(nameEnd));
					if (!name.empty() && IsIdentStart(name.front()) && StartsWith(tail, "="))
					{
						const std::string_view value = Trim(tail.substr(1));
						if (value == "{}")
						{
							currentClass = ensureClass(name, std::string_view());
							addItem(name, std::string_view(), blockDoc,
								LuauCompletionItem::KindType::Global);
						}
					}
				}
			}
			resetBlock();
		}
	}

	void LuauCompletionIndex::ParseFileText(std::string_view text)
	{
		bool inCommentBlock = false;
		bool docTaken = false;
		std::string blockDoc;
		// E2:文件内函数的 `---@return`(与存根同一口径)—— `local y = someCall()` 用它推返回值类型。
		std::string blockReturn;
		std::size_t currentClass = kNone;
		// V9:文件符号每次都整份替换,注解字段表跟着一起重建(SetFileSource 的语义)。
		m_AnnotationFields.clear();

		auto resetBlock = [&]()
		{
			inCommentBlock = false;
			docTaken = false;
			blockDoc.clear();
			blockReturn.clear();
		};

		auto ensureFileClass = [&](std::string_view name, std::string_view base) -> std::size_t
		{
			for (std::size_t i = 0; i < m_FileClasses.size(); ++i)
			{
				if (m_FileClasses[i].Name == name)
				{
					if (m_FileClasses[i].Base.empty() && !base.empty())
						m_FileClasses[i].Base.assign(base);
					return i;
				}
			}
			ClassInfo info;
			info.Name.assign(name);
			info.Base.assign(base);
			m_FileClasses.push_back(std::move(info));
			return m_FileClasses.size() - 1;
		};

		auto addFileItem = [&](std::string_view name, std::string_view type,
			std::string_view doc, LuauCompletionItem::KindType kind)
		{
			for (LuauCompletionItem& item : m_FileItems)
			{
				if (item.Name == name)
				{
					if (item.Kind == LuauCompletionItem::KindType::Class
						&& kind == LuauCompletionItem::KindType::Global)
					{
						item.Kind = LuauCompletionItem::KindType::Global;
						if (item.Type.empty())
							item.Type.assign(type);
						if (item.Doc.empty())
							item.Doc.assign(doc);
					}
					return;
				}
			}
			LuauCompletionItem item;
			item.Name.assign(name);
			item.Type.assign(type);
			item.Doc.assign(doc);
			item.Kind = kind;
			m_FileItems.push_back(std::move(item));
		};

		auto addFileMember = [&](std::size_t classIndex, std::string_view name,
			std::string_view type, std::string_view doc, LuauCompletionItem::KindType kind)
		{
			ClassInfo& info = m_FileClasses[classIndex];
			for (const LuauCompletionItem& member : info.Members)
				if (member.Name == name)
					return;
			LuauCompletionItem item;
			item.Name.assign(name);
			item.Type.assign(type);
			item.Doc.assign(doc);
			item.Kind = kind;
			info.Members.push_back(std::move(item));
		};

		std::size_t lineStart = 0;
		while (lineStart <= text.size())
		{
			const std::size_t lineEnd = text.find('\n', lineStart);
			const bool lastLine = lineEnd == kNone;
			const std::string_view line = Trim(text.substr(lineStart,
				(lastLine ? text.size() : lineEnd) - lineStart));
			lineStart = lastLine ? text.size() + 1 : lineEnd + 1;

			if (StartsWith(line, "---"))
			{
				if (!inCommentBlock)
				{
					inCommentBlock = true;
					docTaken = false;
					blockDoc.clear();
					blockReturn.clear();
				}
				const std::string_view body = line.substr(3);
				if (StartsWith(body, "@"))
				{
					const std::string_view tag = FirstWord(body.substr(1));
					const std::string_view rest = Trim(body.substr(1 + tag.size()));
					if (tag == "class")
					{
						const std::string_view name = FirstWord(rest);
						const std::string_view tail = Trim(rest.substr(name.size()));
						std::string_view base;
						if (!tail.empty() && tail.front() == ':')
							base = FirstWord(Trim(tail.substr(1)));
						if (!name.empty() && IsIdentStart(name.front()))
						{
							currentClass = ensureFileClass(name, base);
							addFileItem(name, std::string_view(), blockDoc,
								LuauCompletionItem::KindType::Class);
						}
					}
					else if (tag == "field")
					{
						const FieldSpec spec = ParseFieldSpec(rest);
						if (!spec.Name.empty() && IsIdentStart(spec.Name.front()))
						{
							// V9:注解字段**总是**记一份(悬停 / 裸名解析用)——脚本不一定先写
							// `---@class`(用户实测:只有 `---@field` 时鼠标停在字段名上没有任何提示)。
							// 与类成员是两回事:类作用域内再额外进成员表(给 `self.` 列表用)。
							bool known = false;
							for (const LuauCompletionItem& existing : m_AnnotationFields)
								if (existing.Name == spec.Name)
								{
									known = true;
									break;
								}
							if (!known)
							{
								LuauCompletionItem field;
								field.Name.assign(spec.Name);
								field.Type.assign(spec.Type);
								field.Doc.assign(spec.Desc);
								field.Kind = LuauCompletionItem::KindType::Field;
								m_AnnotationFields.push_back(std::move(field));
							}
							if (currentClass != kNone)
								addFileMember(currentClass, spec.Name, spec.Type, spec.Desc,
									LuauCompletionItem::KindType::Field);
						}
					}
					else if (tag == "return")
					{
						if (blockReturn.empty())
						{
							const std::string_view type = FirstWord(rest);
							if (!type.empty())
								blockReturn.assign(type);
						}
					}
				}
				else
				{
					const std::string_view prose = Trim(body);
					if (!docTaken && !prose.empty())
					{
						blockDoc.assign(prose);
						docTaken = true;
					}
				}
				continue;
			}

			if (!line.empty() && !StartsWith(line, "--"))
			{
				if (StartsWithWord(line, "local"))
				{
					std::string_view rest = Trim(line.substr(5));
					const bool localFunction = StartsWithWord(rest, "function");
					if (localFunction)
						rest = Trim(rest.substr(8));
					std::size_t nameEnd = 0;
					while (nameEnd < rest.size() && IsIdentPart(rest[nameEnd]))
						++nameEnd;
					const std::string_view name = rest.substr(0, nameEnd);
					if (!name.empty() && IsIdentStart(name.front()))
						addFileItem(name, localFunction && !blockReturn.empty()
								? std::string_view(blockReturn) : std::string_view(),
							blockDoc,
							LuauCompletionItem::KindType::Global);
				}
				else if (StartsWithWord(line, "function"))
				{
					const std::string_view rest = Trim(line.substr(8));
					std::size_t ownerEnd = 0;
					while (ownerEnd < rest.size() && IsIdentPart(rest[ownerEnd]))
						++ownerEnd;
					const std::string_view owner = rest.substr(0, ownerEnd);
					if (owner.empty() || !IsIdentStart(owner.front()))
					{
						resetBlock();
						continue;
					}
					if (ownerEnd < rest.size() && (rest[ownerEnd] == ':' || rest[ownerEnd] == '.'))
					{
						std::size_t methodEnd = ownerEnd + 1;
						while (methodEnd < rest.size() && IsIdentPart(rest[methodEnd]))
							++methodEnd;
						const std::string_view method =
							rest.substr(ownerEnd + 1, methodEnd - (ownerEnd + 1));
						if (!method.empty())
							addFileMember(ensureFileClass(owner, std::string_view()), method,
								blockReturn, blockDoc, LuauCompletionItem::KindType::Method);
					}
					else
					{
						// `---@return T` 写在函数头上时,Type = T(调用表达式推断);没写仍是 function。
						addFileItem(owner, blockReturn.empty() ? std::string_view("function") : blockReturn,
							blockDoc, LuauCompletionItem::KindType::Global);
					}
				}
				else
				{
					std::size_t nameEnd = 0;
					while (nameEnd < line.size() && IsIdentPart(line[nameEnd]))
						++nameEnd;
					const std::string_view name = line.substr(0, nameEnd);
					const std::string_view tail = Trim(line.substr(nameEnd));
					if (!name.empty() && IsIdentStart(name.front()) && StartsWith(tail, "="))
					{
						const std::string_view value = Trim(tail.substr(1));
						if (StartsWith(value, "{"))
						{
							currentClass = ensureFileClass(name, std::string_view());
							addFileItem(name, std::string_view(), blockDoc,
								LuauCompletionItem::KindType::Global);
						}
					}
				}
			}
			resetBlock();
		}
	}

	void LuauCompletionIndex::SetFileSource(std::string_view fileText)
	{
		m_FileItems.clear();
		m_FileClasses.clear();
		// V9b:注解字段表也随"换一份文件"清空 —— 空文本走的是下面的提前返回分支,
		// 不能只依赖 ParseFileText 里的清表(否则上一份文件的 `---@field` 会残留成幽灵提示)。
		m_AnnotationFields.clear();
		m_Inferred.clear();   // D3:推断表同样整份重建
		if (!Trim(fileText).empty())
		{
			ParseFileText(fileText);
			// D3:局部变量/表字段的类型推断(与注解解析分开一趟:表构造可能跨行)。
			ParseInferredLocals(fileText);
		}
	}

	// VEC-H1:文件内符号的只读快照(高亮区分"数据字段/局部"与全局服务表用)。
	// 只读 —— 不改任何索引状态;高亮侧不内置第二份名单,名单只从这里流出去。
	void LuauCompletionIndex::CollectFileSymbols(std::vector<std::string>& outLocals,
		std::vector<std::string>& outFields) const
	{
		outLocals.clear();
		outFields.clear();
		// 局部/文件符号:`local x` / `function x` / `X = {}`(Kind=Global)。
		// `---@class X` 只有类型名、没有运行时值(Kind=Class)→ 不进"局部"档。
		for (const LuauCompletionItem& item : m_FileItems)
			if (item.Kind == LuauCompletionItem::KindType::Global)
				outLocals.push_back(item.Name);
		// 数据字段:`---@field` 注解字段 + 推断表里的字段(表构造键 / 字段赋值 / 子表递归)。
		for (const LuauCompletionItem& field : m_AnnotationFields)
			outFields.push_back(field.Name);
		std::function<void(const std::vector<InferredSymbol>&)> collectFields;
		collectFields = [&collectFields, &outFields](const std::vector<InferredSymbol>& fields)
		{
			for (const InferredSymbol& field : fields)
			{
				outFields.push_back(field.Name);
				collectFields(field.Fields);
			}
		};
		for (const InferredSymbol& symbol : m_Inferred)
			collectFields(symbol.Fields);
	}

	std::size_t LuauCompletionIndex::SymbolCount() const
	{
		std::size_t count = m_Items.size() + m_FileItems.size();
		for (const ClassInfo& info : m_StubClasses)
			count += info.Members.size();
		for (const ClassInfo& info : m_FileClasses)
			count += info.Members.size();
		return count;
	}

	const LuauCompletionIndex::ClassInfo* LuauCompletionIndex::ResolveClass(
		std::string_view name) const
	{
		if (name.empty())
			return nullptr;
		for (const ClassInfo& info : m_StubClasses)
			if (info.Name == name)
				return &info;
		for (const ClassInfo& info : m_FileClasses)
			if (info.Name == name)
				return &info;
		// 大小写不敏感兜底:脚本里常见的 `entity:GetComponent(...)`(小写局部名)仍能给出
		// Entity 的成员;精确匹配永远优先。
		for (const ClassInfo& info : m_StubClasses)
			if (EqualsIgnoreCase(info.Name, name))
				return &info;
		for (const ClassInfo& info : m_FileClasses)
			if (EqualsIgnoreCase(info.Name, name))
				return &info;
		return nullptr;
	}

	void LuauCompletionIndex::CollectClassMembers(const ClassInfo* info,
		std::vector<const LuauCompletionItem*>& out) const
	{
		std::vector<std::string> visited;
		int depth = 0;
		while (info && depth < 16)
		{
			++depth;
			if (std::find(visited.begin(), visited.end(), info->Name) != visited.end())
				break;   // 基类环:已访问过就停
			visited.push_back(info->Name);
			for (const LuauCompletionItem& member : info->Members)
				out.push_back(&member);
			if (info->Base.empty())
				break;
			info = ResolveClass(info->Base);
		}
	}

	// E2:精确大小写的类查找(存根 + 当前文件)。表达式推断不享受 ResolveClass 的不敏感兜底 ——
	// `level.Primary()` 里的 `level` 不能因为存根有 `Level` 服务就被当成类。
	const LuauCompletionIndex::ClassInfo* LuauCompletionIndex::FindClassExact(std::string_view name) const
	{
		if (name.empty())
			return nullptr;
		for (const ClassInfo& info : m_StubClasses)
			if (info.Name == name)
				return &info;
		for (const ClassInfo& info : m_FileClasses)
			if (info.Name == name)
				return &info;
		return nullptr;
	}

	const LuauCompletionItem* LuauCompletionIndex::FindMember(const ClassInfo* info,
		std::string_view name) const
	{
		if (!info)
			return nullptr;
		std::vector<const LuauCompletionItem*> members;
		CollectClassMembers(info, members);
		for (const LuauCompletionItem* member : members)
			if (member->Name == name)
				return member;
		return nullptr;
	}

	void LuauCompletionIndex::CollectGlobals(std::vector<const LuauCompletionItem*>& out,
		bool includeKeywords) const
	{
		out.reserve(out.size() + m_Items.size() + m_FileItems.size());
		for (const LuauCompletionItem& item : m_Items)
		{
			if (!includeKeywords && item.Kind == LuauCompletionItem::KindType::Keyword)
				continue;
			out.push_back(&item);
		}
		for (const LuauCompletionItem& item : m_FileItems)
			out.push_back(&item);
	}

	void LuauCompletionIndex::Query(std::string_view linePrefix, std::size_t maxItems,
		std::vector<LuauCompletionItem>& out) const
	{
		out.clear();

		// 注解上下文:`---@` 之后到光标之间没有空白 → 只给注解标签(用户实测:"注释的 @
		// 后面的关键字没有智能提示")。标签名不带 '@',接受后光标停在标签后。
		{
			const std::size_t at = linePrefix.rfind("---@");
			if (at != std::string_view::npos)
			{
				const std::string_view tail = linePrefix.substr(at + 4);
				bool isTagPrefix = true;
				for (const char c : tail)
					if (!IsIdentPart(c))
						isTagPrefix = false;
				if (isTagPrefix)
				{
					for (const LuauCompletionItem& tag : m_Tags)
						if (StartsWithIgnoreCase(tag.Name, tail))
							out.push_back(tag);
					std::sort(out.begin(), out.end(),
						[](const LuauCompletionItem& l, const LuauCompletionItem& r) { return l.Name < r.Name; });
					if (maxItems != 0 && out.size() > maxItems)
						out.resize(maxItems);
					return;
				}
			}
		}

		// V9:注解的**类型位**(`---@type ` / `---@field <名字> ` / `---@param <名字> ` /
		// `---@class X : `)给 Luau 基础类型 + 已声明的类型/类名(用户反馈:「注释中填类型时没有提示」)。
		{
			std::string_view typePrefix;
			if (IsAnnotationTypePosition(linePrefix, &typePrefix))
			{
				CollectTypeCandidates(typePrefix, out);
				if (maxItems != 0 && out.size() > maxItems)
					out.resize(maxItems);
				return;
			}
		}

		std::string receiver;
		std::string prefix;
		char separator = '\0';
		ParseContext(linePrefix, receiver, separator, prefix);

		std::vector<const LuauCompletionItem*> pool;
		bool dedupe = false;
		if (separator != '\0' && !receiver.empty())
		{
			dedupe = true;   // 基类链/file 类可能有同名成员;成员量小,去重成本可忽略
			if (receiver == "self")
			{
				// self. = 文件类(含基类链)+ WorldScript 的成员并集
				dedupe = true;
				if (!m_FileClasses.empty())
					CollectClassMembers(&m_FileClasses.front(), pool);
				if (const ClassInfo* worldScript = ResolveClass("WorldScript"))
					CollectClassMembers(worldScript, pool);
			}
			else if (const ClassInfo* classInfo = ResolveClass(receiver))
			{
				CollectClassMembers(classInfo, pool);
			}
			else
			{
				// 未知接收者:回退全局 + 文件符号(不给关键字,降低噪声)
				CollectGlobals(pool, false);
			}
		}
		else
		{
			dedupe = !m_FileItems.empty();   // 全局项自身按名字唯一,只有文件符号会撞名
			CollectGlobals(pool, true);
		}

		// 去重(名字大小写不敏感,先到先得)只在可能有重名来源时做;
		// 其余情况直接用指针池,避免每次按键把整表(含长 Doc)复制一遍。
		const std::vector<const LuauCompletionItem*>* candidates = &pool;
		std::vector<const LuauCompletionItem*> unique;
		if (dedupe)
		{
			unique.reserve(pool.size());
			std::unordered_map<std::string, std::size_t> indexByName;
			indexByName.reserve(pool.size() * 2);
			// 同名成员取"信息更全"的一条:文件类里裸写的 `function PlayerScript:OnDestroy()`
			// 没有类型/文档,不能盖掉 WorldScript 注解里带 `fun(self)` 与说明的同名条目
			// (用户实测:列表里 OnDestroy 没有类型也没有注释)。
			const auto infoScore = [](const LuauCompletionItem& item)
			{
				return (item.Doc.empty() ? 0 : 2) + (item.Type.empty() ? 0 : 1);
			};
			for (const LuauCompletionItem* item : pool)
			{
				const std::string key = LowerCopy(item->Name);
				const auto found = indexByName.find(key);
				if (found == indexByName.end())
				{
					indexByName.emplace(key, unique.size());
					unique.push_back(item);
					continue;
				}
				if (infoScore(*item) > infoScore(*unique[found->second]))
					unique[found->second] = item;
			}
			candidates = &unique;
		}

		std::vector<const LuauCompletionItem*> matched;
		matched.reserve(candidates->size());
		for (const LuauCompletionItem* item : *candidates)
			if (StartsWithIgnoreCase(item->Name, prefix))
				matched.push_back(item);
		if (matched.empty() && !prefix.empty())
		{
			for (const LuauCompletionItem* item : *candidates)
				if (ContainsIgnoreCase(item->Name, prefix))
					matched.push_back(item);
		}

		std::sort(matched.begin(), matched.end(),
			[](const LuauCompletionItem* left, const LuauCompletionItem* right)
			{
				const int rankLeft = KindRank(left->Kind);
				const int rankRight = KindRank(right->Kind);
				if (rankLeft != rankRight)
					return rankLeft < rankRight;
				const int nameOrder = CompareIgnoreCase(left->Name, right->Name);
				if (nameOrder != 0)
					return nameOrder < 0;
				return left->Name < right->Name;
			});

		const std::size_t limit = (maxItems == 0) ? matched.size()
			: std::min(matched.size(), maxItems);
		out.reserve(limit);
		for (std::size_t i = 0; i < limit; ++i)
			out.push_back(*matched[i]);
	}

	// ---- D3(2026-09-27 用户口径:悬浮提示要能给出局部变量/表字段的类型) ----
	//
	// 纯文本层,不引入 VM:`SetFileSource` 时按整份源码扫一遍
	//   * `local x = <字面量 | 表构造 | X.new(...)>` → x 的类型(表构造连字段一起记);
	//   * `x = <…>`(x 是已登记的局部)→ 刷新类型(作用域内赋值);
	//   * `t.a.b = <…>`(t 是推断出来的局部表)→ 递归补字段(中间层按 table 自动补);
	//   * `X = { … }`(全局表构造)→ 也登记(未注解字段的悬停/链式解析)。
	// 同名后写覆盖先写(悬停只拿得到"词之前的整行片段",没有行号,只能按最后一次已知回答);
	// 推不出来就不登记 —— 悬停保持现状(不弹空框)。
	const LuauCompletionIndex::InferredSymbol* LuauCompletionIndex::FindInferred(
		std::string_view name) const
	{
		for (auto it = m_Inferred.rbegin(); it != m_Inferred.rend(); ++it)
			if (it->Name == name)
				return &*it;
		return nullptr;
	}

	std::vector<LuauCompletionIndex::InferredSymbol>::iterator LuauCompletionIndex::FindInferred(
		std::string_view name)
	{
		for (auto it = m_Inferred.end(); it != m_Inferred.begin();)
		{
			--it;
			if (it->Name == name)
				return it;
		}
		return m_Inferred.end();
	}

	const LuauCompletionIndex::InferredSymbol* LuauCompletionIndex::FindInferredField(
		const InferredSymbol* symbol, std::string_view name)
	{
		if (!symbol)
			return nullptr;
		for (auto it = symbol->Fields.rbegin(); it != symbol->Fields.rend(); ++it)
			if (it->Name == name)
				return &*it;
		return nullptr;
	}

	// E3②/E4:裸名/接收者链的根解析 —— 当前文件推断表里**任意**结构化表的字段(精确大小写)。
	// 例:`local FX = { ExtraInfo = { note = … } }` 之后悬停裸名 `note`(在表构造字面量里)
	// 或 `ExtraInfo.note` 的链根 `ExtraInfo`。先深后浅、后写优先(与 FindInferred 同一条口径)。
	const LuauCompletionIndex::InferredSymbol* LuauCompletionIndex::FindInferredFieldAnywhere(
		std::string_view name) const
	{
		if (name.empty())
			return nullptr;
		// 先一层(直接字段),再两层(嵌套字段);每一层都按"后写优先"。
		for (auto root = m_Inferred.rbegin(); root != m_Inferred.rend(); ++root)
			if (const InferredSymbol* field = FindInferredField(&*root, name))
				if (!field->Type.empty())
					return field;
		for (auto root = m_Inferred.rbegin(); root != m_Inferred.rend(); ++root)
			for (auto field = root->Fields.rbegin(); field != root->Fields.rend(); ++field)
				if (const InferredSymbol* nested = FindInferredField(&*field, name))
					if (!nested->Type.empty())
						return nested;
		return nullptr;
	}

	void LuauCompletionIndex::ParseInferredLocals(std::string_view text)
	{
		m_Inferred.clear();
		const std::string masked = MaskCommentsAndStrings(text);
		if (masked.empty())
			return;

		// 表构造里的字段(递归):`{ x = 1.5, stats = { hp = 10 } }`。
		std::function<void(std::size_t, int, std::vector<InferredSymbol>&)> parseFields;
		// 表达式 → 类型名(推不出来 = 空串);fields 非空时收表构造里的字段。
		std::function<std::string(std::size_t, int, std::vector<InferredSymbol>*)> inferType;

		parseFields = [&](std::size_t brace, int depth, std::vector<InferredSymbol>& out)
		{
			if (depth > 4)
				return;
			std::size_t pos = brace + 1;
			while (pos < masked.size())
			{
				pos = SkipBlanksAt(masked, pos);
				while (pos < masked.size() && (masked[pos] == ',' || masked[pos] == ';'))
					pos = SkipBlanksAt(masked, pos + 1);
				if (pos >= masked.size() || masked[pos] == '}')
					break;
				if (!IsIdentStart(masked[pos]))
				{
					// [expr] = value / 位置值:跳过(畸形输入兜底:一定要前进)
					const std::size_t before = pos;
					pos = SkipValueExpression(masked, pos);
					if (pos == before)
						++pos;
					continue;
				}
				std::size_t nameEnd = pos;
				while (nameEnd < masked.size() && IsIdentPart(masked[nameEnd]))
					++nameEnd;
				const std::string fieldName = masked.substr(pos, nameEnd - pos);
				std::size_t after = SkipBlanksAt(masked, nameEnd);
				if (after >= masked.size() || masked[after] != '=' ||
					(after + 1 < masked.size() && masked[after + 1] == '='))
				{
					// 位置值:跳过(畸形输入兜底:一定要前进)
					const std::size_t before = pos;
					pos = SkipValueExpression(masked, pos);
					if (pos == before)
						++pos;
					continue;
				}
				std::vector<InferredSymbol> nested;
				const std::string type = inferType(after + 1, depth + 1, &nested);
				if (!type.empty())
				{
					InferredSymbol field;
					field.Name = fieldName;
					field.Type = type;
					field.Fields = std::move(nested);
					const auto existing = std::find_if(out.begin(), out.end(),
						[&fieldName](const InferredSymbol& item) { return item.Name == fieldName; });
					if (existing != out.end())
						*existing = std::move(field);
					else
						out.push_back(std::move(field));
				}
				pos = SkipValueExpression(masked, after + 1);
			}
		};

		inferType = [&](std::size_t pos, int depth, std::vector<InferredSymbol>* fields) -> std::string
		{
			if (fields)
				fields->clear();
			pos = SkipBlanksAt(masked, pos);
			if (pos >= masked.size() || depth > 4)
				return std::string();
			const char first = masked[pos];
			if (first == '{')
			{
				if (fields)
					parseFields(pos, depth, *fields);
				return "table";
			}
			if (first == '"' || first == '\'')
				return "string";
			if (first == '[' && pos + 1 < masked.size() && masked[pos + 1] == '[')
				return "string";   // 长字符串 [[…]]
			if (first >= '0' && first <= '9')
			{
				std::size_t probe = pos;
				while (probe < masked.size() && (IsIdentPart(masked[probe]) || masked[probe] == '.'))
					++probe;
				return "number";   // 十进制/浮点/十六进制字面量
			}
			if ((first == '-' || first == '+') && pos + 1 < masked.size())
			{
				const std::size_t next = SkipBlanksAt(masked, pos + 1);
				if (next < masked.size() && masked[next] >= '0' && masked[next] <= '9')
				{
					std::size_t probe = next;
					while (probe < masked.size() && (IsIdentPart(masked[probe]) || masked[probe] == '.'))
						++probe;
					return "number";
				}
				return std::string();
			}
			if (!IsIdentStart(first))
				return std::string();
			std::size_t end = pos;
			while (end < masked.size() && IsIdentPart(masked[end]))
				++end;
			const std::string name = masked.substr(pos, end - pos);
			if (name == "true" || name == "false")
				return "boolean";
			if (name == "nil")
				return "nil";
			if (name == "function")
				return "function";
			// `X.new(...)`:类型名 = X(只认索引里真有的 `---@class`,不凭名字硬造)。
			const std::size_t after = SkipBlanksAt(masked, end);
			if (after < masked.size() && masked[after] == '.' && ResolveClass(name))
			{
				std::size_t memberEnd = SkipBlanksAt(masked, after + 1);
				std::size_t cursor = memberEnd;
				while (cursor < masked.size() && IsIdentPart(masked[cursor]))
					++cursor;
				if (masked.substr(memberEnd, cursor - memberEnd) == "new")
					return name;
			}

			// ---- E2(2026-09-27 用户口径):成员表达式 / 函数调用结果 ----
			// `a.b` / `self.ExtraInfo.note` / `recv:Method(...)` / `recv.Method(...)` / `f(...)`。
			// 根 = 本文件推断出的表或局部 / `self`(文件类)/ 已声明类名;每段按**精确大小写**取成员,
			// 段的类型就是该成员的类型(`---@field` 或方法上的 `---@return`)。推不出来 → 空(不猜)。
			{
				const LuauCompletionIndex& lookup =
					*static_cast<const LuauCompletionIndex*>(this);   // 走 const 查询重载
				const InferredSymbol* tableSymbol = nullptr;   // 当前值是推断表时指向它(可查字段)
				std::string type;                              // 当前值的类型名
				bool resolved = false;
				// 文件里的 `local function f` / `function f` 带 `---@return` 注解时,Type 就是返回类型
				// (`function` 只是"不知道签名"的旧兜底)——这种情况要能支撑 `local y = f()`。
				// 顺序:推断表(带字段,链式解析要用)→ 文件函数/符号的注解类型 → 推断出的局部类型。
				const InferredSymbol* root = lookup.FindInferred(name);
				if (root && root->Type == "table")
				{
					type = root->Type;
					tableSymbol = root;
					resolved = true;
				}
				else
				{
					for (const LuauCompletionItem& item : m_FileItems)
					{
						if (item.Name == name && !item.Type.empty())
						{
							type = item.Type;
							resolved = true;
							break;
						}
					}
					if (!resolved && root && !root->Type.empty())
					{
						type = root->Type;
						resolved = true;
					}
				}
				if (!resolved && name == "self" && !m_FileClasses.empty())
				{
					if (const InferredSymbol* classTable = lookup.FindInferred(m_FileClasses.front().Name))
					{
						type = classTable->Type;
						tableSymbol = (type == "table") ? classTable : nullptr;
						resolved = !type.empty();
					}
					if (!resolved)
					{
						type = m_FileClasses.front().Name;
						resolved = true;
					}
				}
				if (!resolved && FindClassExact(name))
				{
					type = name;
					resolved = true;
				}
				std::size_t cursor = end;
				while (resolved)
				{
					const std::size_t separatorPos = SkipBlanksAt(masked, cursor);
					if (separatorPos >= masked.size()
						|| (masked[separatorPos] != '.' && masked[separatorPos] != ':'))
						break;
					const std::size_t memberStart = SkipBlanksAt(masked, separatorPos + 1);
					std::size_t memberEnd = memberStart;
					while (memberEnd < masked.size() && IsIdentPart(masked[memberEnd]))
						++memberEnd;
					if (memberEnd == memberStart || !IsIdentStart(masked[memberStart]))
					{
						resolved = false;
						break;
					}
					const std::string member = masked.substr(memberStart, memberEnd - memberStart);
					if (tableSymbol)
					{
						const InferredSymbol* field = FindInferredField(tableSymbol, member);
						if (!field || field->Type.empty())
						{
							resolved = false;
							break;
						}
						type = field->Type;
						tableSymbol = (type == "table") ? field : nullptr;
					}
					else
					{
						const LuauCompletionItem* item = FindMember(FindClassExact(type), member);
						if (!item || item->Type.empty())
						{
							resolved = false;
							break;
						}
						type = item->Type;
					}
					cursor = memberEnd;
				}
				// 表达式本身就是表(`local y = FX.ExtraInfo`)→ 字段表一起带走(链式悬停继续可用)。
				if (resolved && type == "table" && fields && tableSymbol)
					*fields = tableSymbol->Fields;
				// 调用:表达式后面紧跟 '(' → 结果 = 被调成员的返回类型(联合取第一个非 nil)。
				if (resolved)
				{
					const std::size_t callPos = SkipBlanksAt(masked, cursor);
					if (callPos < masked.size() && masked[callPos] == '(')
						type = FirstReturnType(type);
					if (!type.empty())
						return type;
				}
			}
			return std::string();
		};

		// 表字段的写入(同名字段覆盖)。
		const auto setField = [](InferredSymbol& container, const std::string& name,
			const std::string& type, std::vector<InferredSymbol> fields)
		{
			for (InferredSymbol& field : container.Fields)
			{
				if (field.Name == name)
				{
					field.Type = type;
					field.Fields = std::move(fields);
					return;
				}
			}
			InferredSymbol field;
			field.Name = name;
			field.Type = type;
			field.Fields = std::move(fields);
			container.Fields.push_back(std::move(field));
		};
		const auto ensureTableField = [&setField](InferredSymbol& container, const std::string& name)
			-> InferredSymbol*
		{
			for (InferredSymbol& field : container.Fields)
				if (field.Name == name)
					return &field;
			InferredSymbol field;
			field.Name = name;
			field.Type = "table";
			container.Fields.push_back(std::move(field));
			return &container.Fields.back();
		};
		const auto appendSymbol = [this](std::string name, std::string type,
			std::vector<InferredSymbol> fields)
		{
			InferredSymbol symbol;
			symbol.Name = std::move(name);
			symbol.Type = std::move(type);
			symbol.Fields = std::move(fields);
			// D3:同名文件符号(ParseFileText 登记的 `local x` / `X = {}`)顺手补上类型 ——
			// Query/补全列表的 Type 列同样受益(首次登记的类型保留,不来回覆盖)。
			for (LuauCompletionItem& item : m_FileItems)
			{
				if (item.Name == symbol.Name)
				{
					if (item.Type.empty() && !symbol.Type.empty())
						item.Type = symbol.Type;
					break;
				}
			}
			m_Inferred.push_back(std::move(symbol));
		};

		std::size_t index = 0;
		while (index < masked.size())
		{
			if (IsBlank(masked[index]))
			{
				++index;
				continue;
			}
			if (!IsIdentStart(masked[index]))
			{
				index = NextLineStart(masked, index);
				continue;
			}

			const std::string_view view(masked);
			const bool isLocal = StartsWithWord(view.substr(index), "local");
			std::size_t cursor = isLocal ? SkipBlanksAt(masked, index + 5) : index;
			const bool functionDecl = StartsWithWord(view.substr(cursor), "function");
			if (functionDecl)
				cursor = SkipBlanksAt(masked, cursor + 8);
			std::size_t nameEnd = cursor;
			while (nameEnd < masked.size() && IsIdentPart(masked[nameEnd]))
				++nameEnd;
			if (nameEnd == cursor || !IsIdentStart(masked[cursor]))
			{
				index = NextLineStart(masked, index);
				continue;
			}
			const std::string name = masked.substr(cursor, nameEnd - cursor);
			std::size_t after = SkipBlanksAt(masked, nameEnd);
			if (functionDecl)
			{
				// `local function f(...)` / `function f(...)`:类型就是 function。
				// `function X:Method(...)` / `function X.Method(...)`:方法,不登记 X。
				if (isLocal || after >= masked.size() ||
					(masked[after] != ':' && masked[after] != '.'))
					appendSymbol(name, "function", {});
				index = NextLineStart(masked, index);
				continue;
			}
			if (after < masked.size() && (masked[after] == ',' || masked[after] == '('))
			{
				index = NextLineStart(masked, index);   // 多名字列表 / 调用语句:不猜
				continue;
			}
			const bool assignment = after < masked.size() && masked[after] == '=' &&
				(after + 1 >= masked.size() || masked[after + 1] != '=');
			if (assignment)
			{
				std::vector<InferredSymbol> fields;
				const std::string type = inferType(after + 1, 0, &fields);
				if (isLocal)
				{
					appendSymbol(name, type, std::move(fields));
				}
				else if (auto existing = FindInferred(name); existing != m_Inferred.end())
				{
					// 作用域内赋值:`local a = 1` 之后 `a = 2.5` —— 推得出来就刷新。
					if (!type.empty())
					{
						existing->Type = type;
						if (type == "table")
							existing->Fields = std::move(fields);
					}
				}
				index = SkipValueExpression(masked, after + 1);
				continue;
			}
			if (!isLocal && after < masked.size() && masked[after] == '{')
			{
				// 全局表构造 `X = { … }`:登记未注解字段(供链式悬停用;不做补全)。
				std::vector<InferredSymbol> fields;
				const std::string type = inferType(after, 0, &fields);
				if (!type.empty())
					appendSymbol(name, type, std::move(fields));
				index = SkipValueExpression(masked, after);
				continue;
			}
			if (!isLocal && after < masked.size() && masked[after] == '.')
			{
				// 字段赋值 `t.stats.hp = <值>`:根必须是推断出来的局部/全局表。
				std::vector<std::string> chain { name };
				std::size_t chainEnd = after;
				while (chainEnd < masked.size() && masked[chainEnd] == '.')
				{
					const std::size_t segment = SkipBlanksAt(masked, chainEnd + 1);
					std::size_t segmentEnd = segment;
					while (segmentEnd < masked.size() && IsIdentPart(masked[segmentEnd]))
						++segmentEnd;
					if (segmentEnd == segment || !IsIdentStart(masked[segment]))
						break;
					chain.push_back(masked.substr(segment, segmentEnd - segment));
					chainEnd = SkipBlanksAt(masked, segmentEnd);
				}
				if (chain.size() >= 2 && chainEnd < masked.size() && masked[chainEnd] == '=' &&
					(chainEnd + 1 >= masked.size() || masked[chainEnd + 1] != '='))
				{
					const auto root = FindInferred(chain.front());
					if (root != m_Inferred.end() && root->Type == "table")
					{
						std::vector<InferredSymbol> fields;
						const std::string type = inferType(chainEnd + 1, 0, &fields);
						if (!type.empty())
						{
							InferredSymbol* container = &*root;
							for (std::size_t step = 1; container && step + 1 < chain.size(); ++step)
								container = ensureTableField(*container, chain[step]);
							if (container)
								setField(*container, chain.back(), type, std::move(fields));
						}
					}
				}
				index = SkipValueExpression(masked, chainEnd + 1);
				continue;
			}
			if (isLocal)
			{
				// `local a`(没有初值):登记空类型;后续 `a = 2.5` 能补上(推不出来就不弹)。
				appendSymbol(name, std::string(), {});
			}
			index = NextLineStart(masked, index);
		}
	}

	bool LuauCompletionIndex::Describe(std::string_view linePrefix, std::string_view word,
		LuauCompletionItem& out) const
	{
		if (word.empty())
			return false;
		// 直接复用 Query 的上下文/合并/排序:同名成员已经按"信息更全"合并过,
		// 这里只挑出与 word 同名的那一条。
		std::vector<LuauCompletionItem> items;
		Query(linePrefix, 0, items);
		// D3:名字命中但没有类型/文档的项(文件内符号的默认形态)先留作兜底 ——
		// 下面的类型推断能给同一个名字补上类型时优先用它;补不上再原样返回(旧行为)。
		LuauCompletionItem untyped;
		auto enrichMethodType = [](LuauCompletionItem& item)
		{
			if (item.Kind == LuauCompletionItem::KindType::Method && !item.Params.empty())
			{
				std::string sig = "fun(";
				for (std::size_t i = 0; i < item.Params.size(); ++i)
				{
					if (i > 0) sig += ", ";
					sig += item.Params[i];
					if (i < item.ParamTypes.size() && !item.ParamTypes[i].empty())
					{
						sig += ": ";
						sig += item.ParamTypes[i];
					}
				}
				sig += ")";
				if (!item.Type.empty())
				{
					sig += ": ";
					sig += item.Type;
				}
				item.Type = std::move(sig);
			}
		};

		const auto matchExactName = [&items, &untyped, word, &out, &enrichMethodType]() -> bool
		{
			for (const LuauCompletionItem& item : items)
				if (item.Name == word)
				{
					if (!item.Type.empty() || !item.Doc.empty() || !item.Params.empty())
					{
						out = item;
						enrichMethodType(out);
						return true;
					}
					if (untyped.Name.empty())
						untyped = item;
				}
			return false;
		};
		// VEC-F1(2026-09-27 用户口径「Level 也要修复」):"本文件推断"与"名字命中"的先后按
		// **接收者是不是类系统认识的名字**决定(ParseContext 的 receiver 只取分隔符前紧邻的一段,
		// `a.b.` 的接收者是 `b`):
		//   * 认识(self 有文件类 / 存根或文件里声明过的类)→ 类成员(`---@field`/方法,带 Doc)
		//     先于本文件推断:`self.Speed` / `vec3.new` 的注解说明保持原样;
		//   * 不认识(`self.InferredStats.` 的 `InferredStats` 只是表构造字段)或没有接收者
		//     (裸 `Level`)→ 名字命中推迟到本文件推断之后(下面的 D3/E3② 块),否则存根里同名的
		//     `---@class Level` 服务表会把文件内 `Level` 字段的提示抢走(E1 只修了大小写不同的 `level`)。
		std::string contextReceiver;
		std::string contextPrefix;
		char contextSeparator = '\0';
		ParseContext(linePrefix, contextReceiver, contextSeparator, contextPrefix);
		const bool receiverIsKnownClass = contextSeparator != '\0' && !contextReceiver.empty()
			&& ((contextReceiver == "self" && !m_FileClasses.empty())
				|| ResolveClass(contextReceiver) != nullptr);
		if (receiverIsKnownClass && matchExactName())
			return true;
		// VEC-E1(E4):大小写不敏感的"名字兜底"**不再紧跟在精确匹配后面** —— 它要排在"本文件
		// 自己的推断结果"之后(见下面的 D3/E3② 块)。修复前悬停 `ExtraInfo = { level = 1 }` 里的
		// `level` 会先命中存根里仅大小写不同的 `Level` 全局服务(tooltip 变成 Level 服务的说明,
		// 用户:「很奇怪,不知道从哪读的」)。
		// V9:当前文件注解里声明的字段(`---@field Speed number 移动速度`)。
		//
		// 为什么需要这一步:①悬停在**注解行里的字段名**上时,光标前缀是 `---@field `,Query 给的是
		// 类型候选,拿不到这个字段自己;②脚本没写 `---@class`(只有 `---@field`)时,字段不在
		// 任何类成员里,`self.Speed` / 裸 `Speed` 也解析不到。这里按"当前文件注解"兜底:
		// 接收者成员/全局已经解析到的仍然优先(它们信息更全、上下文更准)。
		for (const LuauCompletionItem& item : m_AnnotationFields)
			if (item.Name == word)
			{
				out = item;
				return true;
			}
		for (const LuauCompletionItem& item : m_AnnotationFields)
			if (EqualsIgnoreCase(item.Name, word))
			{
				out = item;
				return true;
			}
		// V9b:注解行上的**类型名** hover —— word 命中内置类型 / 已声明类型名时给它的说明。
		//
		// 为什么需要:内核传的 linePrefix 是"词之前的整行片段",悬停**字段名**时前缀是 `---@field `(名字位,
		// 本来拿不到类型候选),字段名解析成功后紧跟着 hover 类型名(`number`)也要有解释;
		// 另外 `---@field Speed number` 这种行无论光标落在哪一段,类型名都该能讲清是什么。
		// 只在**行前缀出现 `---@`** 时启用:代码里的同名标识符不能被当成类型讲解。
		// 类型表复用 CollectTypeCandidates(基础类型 + 已声明类型/类名),不另抄第二份。
		if (linePrefix.rfind("---@") != std::string_view::npos)
		{
			std::vector<LuauCompletionItem> types;
			CollectTypeCandidates(word, types);
			for (const LuauCompletionItem& item : types)
				if (item.Name == word)
				{
					out = item;
					return true;
				}
			for (const LuauCompletionItem& item : types)
				if (EqualsIgnoreCase(item.Name, word))
				{
					out = item;
					return true;
				}
		}
		// D3 / E3②(2026-09-27 用户口径:悬浮也要能推出局部变量/表字段的类型;推断结果要参与
		// **接收者链**解析):
		//   * 接收者链 `t.x` / `t.stats.hp` / `self.ExtraInfo.note` / `ExtraInfo.note` —— 悬停字段名时
		//     linePrefix 以 '.' 结尾,从右往左解析标识符链;根按 推断表 → `self`(文件类)→
		//     本文件任意结构化表的字段(如 `ExtraInfo`,它只是 `local X = {...}` 的子字段)解析;
		//   * 裸名 —— `local a = 1` 之后悬停 `a`,或悬停表构造字面量里的字段名(`note` / `level`);
		//   * **优先于**大小写不敏感的全局兜底(E4 的串台修复)。
		// 全部推不出来 → 保持现状(不弹空框)。
		{
			const auto trimmed = [](std::string_view value)
			{
				std::size_t end = value.size();
				while (end > 0 && IsBlank(value[end - 1]))
					--end;
				return value.substr(0, end);
			};

			std::string_view prefix = trimmed(linePrefix);
			if (!prefix.empty() && prefix.back() == '.')
			{
				prefix.remove_suffix(1);
				std::vector<std::string> chain;
				bool valid = true;
				while (true)
				{
					prefix = trimmed(prefix);
					const std::size_t end = prefix.size();
					std::size_t start = end;
					while (start > 0 && IsIdentPart(prefix[start - 1]))
						--start;
					if (start == end || !IsIdentStart(prefix[start]) || chain.size() >= 8)
					{
						valid = false;
						break;
					}
					chain.insert(chain.begin(), std::string(prefix.substr(start, end - start)));
					prefix = trimmed(prefix.substr(0, start));
					if (!prefix.empty() && prefix.back() == '.')
					{
						prefix.remove_suffix(1);
						continue;
					}
					break;
				}
				if (valid && !chain.empty())
				{
					const InferredSymbol* symbol = FindInferred(chain.front());
					// `self.ExtraInfo.note`:self = 文件类的值(推出来的类表,退化成类名)。
					// VEC-F1:文件里可能有多个 `---@class`(子结构体声明在脚本类**之前**,如
					// FeatureShowcase.lua 的 FeatureShowcaseStats)——`self` 要绑**带同名推断表**的
					// 脚本类;`front()` 不一定是它(三层链因此曾解析不出来)。
					if (!symbol && chain.front() == "self" && !m_FileClasses.empty())
					{
						for (const ClassInfo& info : m_FileClasses)
						{
							const InferredSymbol* classTable = FindInferred(info.Name);
							if (classTable && classTable->Type == "table")
							{
								symbol = classTable;
								break;
							}
						}
						if (!symbol)
							symbol = FindInferred(m_FileClasses.front().Name);
					}
					// `ExtraInfo.note`:ExtraInfo 只是 `local X = { ExtraInfo = {...} }` 的子字段。
					if (!symbol)
						symbol = FindInferredFieldAnywhere(chain.front());
					for (std::size_t step = 1; symbol && step < chain.size(); ++step)
						symbol = FindInferredField(symbol, chain[step]);
					if (symbol && symbol->Type == "table")
					{
						const InferredSymbol* field = FindInferredField(symbol, word);
						if (field && !field->Type.empty())
						{
							out.Name.assign(word);
							out.Type = field->Type;
							out.Kind = LuauCompletionItem::KindType::Field;
							return true;
						}
					}
				}
			}

			if (const InferredSymbol* local = FindInferred(word))
			{
				if (!local->Type.empty())
				{
					out.Name.assign(word);
					out.Type = local->Type;
					out.Kind = LuauCompletionItem::KindType::Global;
					return true;
				}
			}
			// 裸名命中"本文件某个表构造里的字段"(悬停字面量里的 `note` / `level`)。
			if (const InferredSymbol* field = FindInferredFieldAnywhere(word))
			{
				out.Name.assign(word);
				out.Type = field->Type;
				out.Kind = LuauCompletionItem::KindType::Field;
				return true;
			}
		}
		// VEC-F1:接收者未知/没有接收者时,精确的名字命中被推迟到这里 —— 本文件推断没答上
		// 再走存根/全局/文件符号(已知类接收者在上面已经查过)。
		if (!receiverIsKnownClass && matchExactName())
			return true;
		// 大小写不敏感的名字兜底(VEC-E1:排在推断之后 —— 只在"本文件没有任何自己的解释"时才生效)。
		for (const LuauCompletionItem& item : items)
			if (EqualsIgnoreCase(item.Name, word))
			{
				if (!item.Type.empty() || !item.Doc.empty() || !item.Params.empty())
				{
					out = item;
					enrichMethodType(out);
					return true;
				}
				if (untyped.Name.empty())
					untyped = item;
			}
		// 名字命中但类型/文档都推不出来 → 维持旧行为(返回那条空项,不弹新框)。
		if (!untyped.Name.empty())
		{
			out = untyped;
			enrichMethodType(out);
			return true;
		}
		return false;
	}

	// V9:注解类型位的候选 = 常用基础类型 → 引擎类型(vec2/vec3/vec4/mat3/mat4/Entity/WorldScript)
	// → 其余基础类型 → 索引里其余已声明的类型/类名(存根 + 当前文件,按字典序)。
	// D1(2026-09-26):原来的 13 条基础类型固定排在最前,把引擎类型挤出首屏 —— 实测存根 36 个类时
	// vec2/vec3/vec4 在 49 个候选里排第 46/47/48 位(WorldScript 第 49 位);现在前 13 条 =
	// 常用基础(6)+ 引擎类型(7),冷门基础类型仍可打前缀命中(不删任何候选)。
	// 前缀按大小写不敏感前缀过滤(类型名短,不做子串兜底)。
	void LuauCompletionIndex::CollectTypeCandidates(std::string_view prefix,
		std::vector<LuauCompletionItem>& out) const
	{
		out.clear();

		const auto appendBase = [&](const AnnotationBaseType& base)
		{
			if (!prefix.empty() && !StartsWithIgnoreCase(base.Name, prefix))
				return;
			LuauCompletionItem item;
			item.Name.assign(base.Name);
			item.Doc.assign(base.Doc);
			item.Kind = LuauCompletionItem::KindType::Keyword;
			out.push_back(std::move(item));
		};
		for (const AnnotationBaseType& base : kAnnotationCommonBaseTypes)
			appendBase(base);

		// 引擎类型档:名单与顺序来自 D1;只有索引里真的声明了这个 `---@class`(存根或当前文件)
		// 才出现 —— 不凭名字硬造候选。Doc 优先取存根注释块,存根没写说明时用兜底一句话。
		const auto findClass = [this](std::string_view name) -> const ClassInfo*
		{
			for (const ClassInfo& info : m_StubClasses)
				if (EqualsIgnoreCase(info.Name, name))
					return &info;
			for (const ClassInfo& info : m_FileClasses)
				if (EqualsIgnoreCase(info.Name, name))
					return &info;
			return nullptr;
		};
		const auto findItemDoc = [this](std::string_view name) -> std::string
		{
			for (const LuauCompletionItem& item : m_Items)
				if (EqualsIgnoreCase(item.Name, name))
					return item.Doc;
			return {};
		};
		for (const AnnotationEngineType& engine : kAnnotationEngineTypes)
		{
			if (!prefix.empty() && !StartsWithIgnoreCase(engine.Name, prefix))
				continue;
			if (!findClass(engine.Name))
				continue;
			std::string doc = findItemDoc(engine.Name);
			if (doc.empty())
				doc = engine.FallbackDoc;
			LuauCompletionItem item;
			item.Name.assign(engine.Name);
			item.Doc = std::move(doc);
			item.Kind = LuauCompletionItem::KindType::Class;
			out.push_back(std::move(item));
		}

		for (const AnnotationBaseType& base : kAnnotationOtherBaseTypes)
			appendBase(base);

		// 其余已声明的类型/类名:名字去重(大小写不敏感;基础/引擎档已占的名字不重复出现);
		// 说明优先取同名符号项里已有的 Doc
		// (存根的 `---@class` 前注释块进的就是它)。
		std::vector<LuauCompletionItem> named;
		const auto addClass = [&](std::string_view name)
		{
			if (name.empty())
				return;
			for (const LuauCompletionItem& existing : named)
				if (EqualsIgnoreCase(existing.Name, name))
					return;
			for (const LuauCompletionItem& existing : out)
				if (EqualsIgnoreCase(existing.Name, name))
					return;   // 基础类型 / 引擎类型档优先(同名类不再进"其余类名")
			LuauCompletionItem item;
			item.Name.assign(name);
			item.Kind = LuauCompletionItem::KindType::Class;
			for (const LuauCompletionItem& source : m_Items)
				if (EqualsIgnoreCase(source.Name, name))
				{
					item.Doc = source.Doc;
					break;
				}
			named.push_back(std::move(item));
		};
		for (const ClassInfo& info : m_StubClasses)
			addClass(info.Name);
		for (const ClassInfo& info : m_FileClasses)
			addClass(info.Name);

		// 基础类型之后按字典序(与索引里其它排序同口径:先大小写不敏感,再逐字节)。
		std::sort(named.begin(), named.end(),
			[](const LuauCompletionItem& left, const LuauCompletionItem& right)
			{
				const int order = CompareIgnoreCase(left.Name, right.Name);
				if (order != 0)
					return order < 0;
				return left.Name < right.Name;
			});
		for (LuauCompletionItem& item : named)
			if (prefix.empty() || StartsWithIgnoreCase(item.Name, prefix))
				out.push_back(std::move(item));
	}

	void LuauCompletionIndex::ParseContext(std::string_view linePrefix, std::string& receiver,
		char& separator, std::string& prefix)
	{
		receiver.clear();
		separator = '\0';
		prefix.clear();

		// 1) prefix = 光标前紧邻的标识符片段(光标前是空白/运算符则为空)。
		const std::size_t cursor = linePrefix.size();
		std::size_t identifierStart = cursor;
		while (identifierStart > 0 && IsIdentPart(linePrefix[identifierStart - 1]))
			--identifierStart;
		if (identifierStart < cursor && IsIdentStart(linePrefix[identifierStart]))
			prefix.assign(linePrefix.substr(identifierStart, cursor - identifierStart));

		// 2) 跳过标识符与前导空白,找 '.'/':'。
		std::size_t beforeSeparator = identifierStart;
		while (beforeSeparator > 0 && IsBlank(linePrefix[beforeSeparator - 1]))
			--beforeSeparator;
		if (beforeSeparator == 0)
			return;
		const char found = linePrefix[beforeSeparator - 1];
		if (found != '.' && found != ':')
			return;

		// 3) receiver = 分隔符前紧邻的标识符(链式 a.b. 取最后一段 b)。
		std::size_t receiverEnd = beforeSeparator - 1;
		while (receiverEnd > 0 && IsBlank(linePrefix[receiverEnd - 1]))
			--receiverEnd;
		std::size_t receiverStart = receiverEnd;
		while (receiverStart > 0 && IsIdentPart(linePrefix[receiverStart - 1]))
			--receiverStart;
		if (receiverStart == receiverEnd || !IsIdentStart(linePrefix[receiverStart]))
			return;

		receiver.assign(linePrefix.substr(receiverStart, receiverEnd - receiverStart));
		separator = found;
	}
}
