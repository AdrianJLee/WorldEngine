#include "World/Script/LuauCompletion.h"

#include <algorithm>
#include <fstream>
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

		// V9:注解类型位的"基础类型"档(固定顺序,排在类型/类名之前)。
		struct AnnotationBaseType
		{
			const char* Name;
			const char* Doc;
		};

		constexpr AnnotationBaseType kAnnotationBaseTypes[] = {
			{ "any", "任意类型(不检查成员)" },
			{ "boolean", "布尔值(true / false)" },
			{ "buffer", "二进制缓冲区" },
			{ "function", "函数(可用 fun(...): ... 细化签名)" },
			{ "integer", "整数(64 位整数值)" },
			{ "never", "永不返回(如 error 的返回类型)" },
			{ "nil", "空值" },
			{ "number", "数值(Luau 的 number)" },
			{ "string", "字符串" },
			{ "table", "表(可用 { [K]: V } 细化)" },
			{ "thread", "协程线程" },
			{ "unknown", "未知类型(使用前需要收窄)" },
			{ "vector", "向量(如 vec3.new 的返回)" },
		};
	}

	void LuauCompletionIndex::Clear()
	{
		m_Items.clear();
		m_ItemByName.clear();
		m_StubClasses.clear();
		m_FileItems.clear();
		m_FileClasses.clear();
		m_AnnotationFields.clear();
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
		std::size_t currentClass = kNone;

		auto resetBlock = [&]()
		{
			inCommentBlock = false;
			docTaken = false;
			blockDoc.clear();
			blockReturn.clear();
			blockParams.clear();
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
			const std::vector<std::string>& params = {})
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
						if (!name.empty() && name != "self" && name != "...")
							blockParams.emplace_back(name);
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
							addMember(ensureClass(owner, std::string_view()), method, blockReturn,
								blockDoc, LuauCompletionItem::KindType::Method, blockParams);
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
		std::size_t currentClass = kNone;
		// V9:文件符号每次都整份替换,注解字段表跟着一起重建(SetFileSource 的语义)。
		m_AnnotationFields.clear();

		auto resetBlock = [&]()
		{
			inCommentBlock = false;
			docTaken = false;
			blockDoc.clear();
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
					if (StartsWithWord(rest, "function"))
						rest = Trim(rest.substr(8));
					std::size_t nameEnd = 0;
					while (nameEnd < rest.size() && IsIdentPart(rest[nameEnd]))
						++nameEnd;
					const std::string_view name = rest.substr(0, nameEnd);
					if (!name.empty() && IsIdentStart(name.front()))
						addFileItem(name, std::string_view(), blockDoc,
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
								std::string_view(), blockDoc, LuauCompletionItem::KindType::Method);
					}
					else
					{
						addFileItem(owner, "function", blockDoc, LuauCompletionItem::KindType::Global);
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
		if (!Trim(fileText).empty())
			ParseFileText(fileText);
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

	bool LuauCompletionIndex::Describe(std::string_view linePrefix, std::string_view word,
		LuauCompletionItem& out) const
	{
		if (word.empty())
			return false;
		// 直接复用 Query 的上下文/合并/排序:同名成员已经按"信息更全"合并过,
		// 这里只挑出与 word 同名的那一条。
		std::vector<LuauCompletionItem> items;
		Query(linePrefix, 0, items);
		for (const LuauCompletionItem& item : items)
			if (item.Name == word)
			{
				out = item;
				return true;
			}
		for (const LuauCompletionItem& item : items)
			if (EqualsIgnoreCase(item.Name, word))
			{
				out = item;
				return true;
			}
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
		return false;
	}

	// V9:注解类型位的候选 = Luau 基础类型(固定顺序,**排在前面**)+ 索引里已声明的类型/类名
	// (存根 + 当前文件的 `---@class`,按字典序)。前缀按大小写不敏感前缀过滤(类型名短,不做子串兜底)。
	void LuauCompletionIndex::CollectTypeCandidates(std::string_view prefix,
		std::vector<LuauCompletionItem>& out) const
	{
		out.clear();
		for (const AnnotationBaseType& base : kAnnotationBaseTypes)
		{
			if (!prefix.empty() && !StartsWithIgnoreCase(base.Name, prefix))
				continue;
			LuauCompletionItem item;
			item.Name.assign(base.Name);
			item.Doc.assign(base.Doc);
			item.Kind = LuauCompletionItem::KindType::Keyword;
			out.push_back(std::move(item));
		}

		// 已声明的类型/类名:名字去重(大小写不敏感);说明优先取同名符号项里已有的 Doc
		// (存根的 `---@class` 前注释块进的就是它)。
		std::vector<LuauCompletionItem> named;
		const auto addClass = [&](std::string_view name)
		{
			if (name.empty())
				return;
			for (const LuauCompletionItem& existing : named)
				if (EqualsIgnoreCase(existing.Name, name))
					return;
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
