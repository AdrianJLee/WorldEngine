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
	}

	void LuauCompletionIndex::Clear()
	{
		m_Items.clear();
		m_ItemByName.clear();
		m_StubClasses.clear();
		m_FileItems.clear();
		m_FileClasses.clear();
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
		std::size_t currentClass = kNone;

		auto resetBlock = [&]()
		{
			inCommentBlock = false;
			docTaken = false;
			blockDoc.clear();
			blockReturn.clear();
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
			std::string_view doc, LuauCompletionItem::KindType kind)
		{
			ClassInfo& info = m_StubClasses[classIndex];
			for (const LuauCompletionItem& member : info.Members)
				if (member.Name == name)
					return;   // 同名成员保留首个
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
					// @param/@overload/@operator/@meta 与未知 tag:忽略,不打断注释块
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
								blockDoc, LuauCompletionItem::KindType::Method);
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
						if (currentClass != kNone)
						{
							const FieldSpec spec = ParseFieldSpec(rest);
							if (!spec.Name.empty() && IsIdentStart(spec.Name.front()))
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
			std::unordered_set<std::string> seen;
			seen.reserve(pool.size() * 2);
			for (const LuauCompletionItem* item : pool)
				if (seen.insert(LowerCopy(item->Name)).second)
					unique.push_back(item);
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
