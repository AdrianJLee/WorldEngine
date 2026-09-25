#pragma once

// MAT-INTEL(用户 2026-09-24:「接下来做材质编辑器代码的智能提示,格式化以及完善知识库和资料」):
// 材质着色器(`.slang`)的补全 + 悬停文档表 —— 编辑器侧 header-only(与 SlangHighlight.h 同一口径)。
// MAT-INTEL2(用户 2026-09-24:「interface 这种关键字没有提示」):关键字 / 内建 / 引擎契约符号与
// 高亮一样只从 `SlangKeywords.h` 的**唯一一张表**取 —— 旧版补全自己只有 11 条关键字,而高亮认得
// `interface`/`enum`/`where`…,于是"高亮得出、补全不出来"。本文件只留"选哪些候选 / 怎么过滤排序 /
// 悬停给什么文档",词表不再复制。
// MAT-INTEL3(用户 2026-09-24:「函数内局部变量没有提示」):补上**当前缓冲的保守语义索引**
// (形参 / 局部变量 / 文件级声明 / 文件内类型名,见 `SlangSemantics`):候选按光标所在函数作用域
// 过滤,索引里的符号排在词表前面(Kind=Field/Global),悬停给同一份文档。
// MAT-INTEL4(用户 2026-09-24「类型标识颜色应该和名字分开」):高亮**不再**读这份名字集合
// (变量/参数/局部名保持默认色,类型标识走 `SlangKeywords.h` 的 Highlight 类别,见 SlangHighlight.h);
// `DeclaredNames()` 仍由面板传给高亮缓存,单纯为了不改面板接口。
//
// 复用 Wui::CodeEditor 已经具备的补全浮层 / Hover 基础设施(见 WuiCodeEditor.h 的
// `Completion` / `Hover` / `CompletionIdPrefix`):候选与文档用引擎冻结结构 `LuauCompletionItem`
// (Name/Type/Doc/Params/Kind),**不新增引擎接口**,也不改 WuiCodeEditor / LuauCompletionIndex。
//
// 事实源(不要在别处再手写一份):
//   - `MaterialInputs` / `Surface` 的字段名、类型、语义、默认表达式 =
//     `World/Renderer/MaterialSurfaceContract.hlsli` 的 X-macro(MaterialEditorPanel.h 已经
//     通过 World/Renderer/MaterialSurface.h 看到它;这里直接读 `MaterialSurfaceContract::*Fields`);
//   - 关键字 / 内建 / 引擎类型与函数 / 注解词 = `SlangKeywords.h`(高亮读同一张表);
//   - 注解语法 = `docs/dev/shader-contract.md` §6,解析实现在 `SlangKeywords.h` 的 SlangAnnotations;
//   - 本文件声明的 `//! param` 名字来自当前缓冲(SetFileSource)。
//
// 查询口径(与 LuauCompletionIndex 一致,让两个编辑器的候选行为对得上):
//   QueryAt(linePrefix, caretLine) = 上下文(接收者 / 分隔符 / 前缀)→ 候选(成员 | 语义索引
//   (局部/形参/文件级)+ 文件参数 + 引擎函数+类型+内建+关键字 | 注解)→ 过滤(前缀大小写不敏感;
//   前缀零命中时退化为子串匹配)→ 排序(Field→Method→Global→Keyword→Class,同档按名字大小写不敏感
//   升序)→ maxItems 截断。caretLine 只用于**作用域过滤**(局部/形参);<0 = 未知 → 只给文件级符号。
//   Query(linePrefix) = QueryAt(linePrefix, -1) 的兼容入口。
//   Describe(linePrefix, word) = 同一个词的类型与文档(输入/表面字段、文件符号/局部/形参、
//   引擎函数、类型/内建),Hover 与补全共用这一份数据;悬停不做作用域过滤。

#include "SlangHighlight.h"

#include "World/Renderer/MaterialSurfaceContract.hlsli"
#include "World/Script/LuauCompletion.h"
#include "World/WUI/WuiCodeEditor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace World
{
	// MAT-INTEL3:当前缓冲的**保守语义索引** —— 不建 AST,只用"清理后的行 + 大括号深度"做行级判定。
	// 口径(与 `tools/agents/reports/MAT-INTEL3-local-symbols.md` 的"索引口径/作用域裁决"一致):
	//   - 文件级声明 / 辅助函数名 / struct·interface·class·enum 名 → 全文件可用(Origin::FileScope);
	//   - 函数形参 → 只在所属函数体内可见(Origin::Parameter);
	//   - 函数体里的局部变量 → 同一函数体内、声明行**之后**(含同一行)可见(Origin::Local);
	//   - struct/interface 里的字段与方法 → 不进顶层候选(Origin::Member;着色由类型名/关键字规则决定);
	//   - 跨函数不可见;文件级符号不区分"声明在前还是在后"(一律可用)。
	//
	// 已知的**保守近似**(宁可多给,不静默少给;逐条列在报告"未决"里):
	//   - 只认"已知类型名"开头的声明行(= `SlangKeywords.h` 的 Type/EngineType 条目 + 文件内类型名),
	//     认不出的类型不索引(不猜);
	//   - 一行多个声明(`float a, b;`)只索引第一个名字;跨行签名不索引;`for (int i = …)` 的 `i` 不索引;
	//   - 函数内的嵌套 `{}`(if/for/while)不细分:声明在整个函数体内可见;
	//   - 着色不看作用域(高亮只有"名字"一个维度):索引里的名字在整个文件里都按 Global 上色,
	//     因此跨函数的同名标识符会一起着色 —— 用"少留白字"换"绝不漏色"。
	namespace SlangSemantics
	{
		enum class Origin : unsigned char
		{
			FileScope,  // 文件级声明 / 顶层函数名 / 顶层类型名 —— 全文件可用
			Parameter,  // 函数形参 —— 只在所属函数体内
			Local,      // 函数体内的局部变量 —— 同一函数体内、声明行之后
			Member,     // struct/interface 的字段与方法 —— 只着色,不进顶层候选
		};

		struct Decl
		{
			std::string Name;
			std::string Type;                    // 补全"类型"列(声明里读到的类型文本;类型名条目为空)
			Origin Origin = Origin::FileScope;
			bool Function = false;               // 函数/方法名(文档写 "function returning …")
			bool TypeName = false;               // struct/interface/class/enum 名(文档写 "type")
			int Line = 0;                        // 0 基声明行(形参 = 签名行)
			int ScopeStart = -1;                 // 所属函数体 `{` 的行号(文件级 / 成员 = -1)
			int ScopeEnd = -1;                   // 所属函数体 `}` 的行号
		};

		// 一个函数体(含 struct/interface 里的方法)的行号区间。
		struct Scope
		{
			int Start = 0;
			int End = 0;
		};

		struct FileIndex
		{
			std::vector<Decl> Decls;
			std::vector<Scope> Scopes;
			std::vector<std::string> Names;      // 去重后的全部名字(高亮用,含成员)
		};

		// ---- 词法小工具(只认 ASCII;与 SlangHighlighter 的判定同口径)----
		inline bool IsIdentStart(char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		}
		inline bool IsIdentChar(char c)
		{
			return IsIdentStart(c) || (c >= '0' && c <= '9');
		}
		inline bool IsBlank(char c)
		{
			return c == ' ' || c == '\t' || c == '\r';
		}
		inline std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && IsBlank(text.front()))
				text.remove_prefix(1);
			while (!text.empty() && IsBlank(text.back()))
				text.remove_suffix(1);
			return text;
		}

		// 注释与字符串里的字符替换成空格(偏移与原文一一对应),这样括号计数 / 形态判定不会
		// 被注释或字符串里的括号带偏。块注释状态跨行延续(与高亮同口径)。
		inline std::string CleanLine(std::string_view line, bool& blockComment)
		{
			std::string out(line.size(), ' ');
			std::size_t index = 0;
			while (index < line.size())
			{
				const char c = line[index];
				if (blockComment)
				{
					if (c == '*' && index + 1 < line.size() && line[index + 1] == '/')
					{
						blockComment = false;
						index += 2;
						continue;
					}
					++index;
					continue;
				}
				if (c == '/' && index + 1 < line.size() && line[index + 1] == '/')
					break;   // 行注释:余下全是注释
				if (c == '/' && index + 1 < line.size() && line[index + 1] == '*')
				{
					blockComment = true;
					index += 2;
					continue;
				}
				if (c == '"' || c == '\'')
				{
					const char quote = c;
					++index;
					while (index < line.size())
					{
						if (line[index] == '\\' && index + 1 < line.size())
						{
							index += 2;
							continue;
						}
						if (line[index] == quote)
						{
							++index;
							break;
						}
						++index;
					}
					continue;
				}
				out[index] = c;
				++index;
			}
			return out;
		}

		struct Word
		{
			std::string_view Text;
			std::size_t Start = 0;
			std::size_t End = 0;
		};

		inline void Tokenize(std::string_view code, std::vector<Word>& out)
		{
			out.clear();
			std::size_t index = 0;
			while (index < code.size())
			{
				if (!IsIdentStart(code[index]))
				{
					++index;
					continue;
				}
				const std::size_t start = index;
				while (index < code.size() && IsIdentChar(code[index]))
					++index;
				out.push_back(Word { code.substr(start, index - start), start, index });
			}
		}

		// 一条形参 / 一处声明。
		struct Param
		{
			std::string Name;
			std::string Type;
		};

		struct Signature
		{
			std::string Name;
			std::string ReturnType;
			std::vector<Param> Params;
			bool HasBody = false;
			bool BraceOnLine = false;
		};

		inline bool MatchesWord(std::string_view code, std::string_view keyword)
		{
			return code.size() > keyword.size() && code.compare(0, keyword.size(), keyword) == 0
				&& IsBlank(code[keyword.size()]);
		}

		// 声明行开头的"类型引导词"(`struct` / `interface` / `using` …):这些行不进语句级声明解析,
		// 否则 `struct TiledUV : IUVTransform` 会被读成"名叫 IUVTransform 的声明"。
		inline bool StartsWithTypeIntro(std::string_view code)
		{
			static const char* const kIntro[] = { "struct", "interface", "class", "enum", "namespace",
				"using", "typedef", "import", "module", "cbuffer", "tbuffer", "public", "private",
				"internal", "static_assert" };
			for (const char* keyword : kIntro)
				if (MatchesWord(code, keyword))
					return true;
			return false;
		}

		// 声明里的类型名:词表的 Type / EngineType(= Slang 标量·向量·矩阵·引擎契约结构)+ 文件内类型名。
		inline bool IsDeclaratorType(std::string_view word, const std::vector<std::string>& fileTypes)
		{
			for (const std::string& name : fileTypes)
				if (word == name)
					return true;
			const SlangSymbols::Symbol* symbol = SlangSymbols::Find(word);
			if (symbol == nullptr)
				return false;
			return symbol->Kind == SlangSymbols::Class::Type
				|| symbol->Kind == SlangSymbols::Class::EngineType;
		}

		// 函数返回类型:声明类型 + `void`(helper 只写出参时很常见)。
		inline bool IsReturnType(std::string_view word, const std::vector<std::string>& fileTypes)
		{
			return word == "void" || IsDeclaratorType(word, fileTypes);
		}

		// `float2 uv`, `MaterialInputs input`, `const float3& tint`, `T transform`, `float x = 1` →
		// 名字 = 最后一个标识符,类型 = 它前面的文本(允许限定词/`*`/`&`/`<`/`>`/`:`);认不出就跳过。
		inline void AddParam(std::string_view text, std::vector<Param>& out)
		{
			const std::size_t equal = text.find('=');
			if (equal != std::string_view::npos)
				text = text.substr(0, equal);
			text = Trim(text);
			if (text.empty() || text == "void")
				return;
			std::size_t end = text.size();
			while (end > 0 && IsBlank(text[end - 1]))
				--end;
			std::size_t start = end;
			while (start > 0 && IsIdentChar(text[start - 1]))
				--start;
			if (start == end || !IsIdentStart(text[start]))
				return;
			const std::string_view name = text.substr(start, end - start);
			std::string_view type = Trim(text.substr(0, start));
			while (!type.empty() && (type.back() == '*' || type.back() == '&'))
			{
				type.remove_suffix(1);
				type = Trim(type);
			}
			if (type.empty())
				return;
			for (char c : type)
				if (!IsIdentChar(c) && !IsBlank(c) && c != ':' && c != '*' && c != '&'
					&& c != '<' && c != '>')
					return;
			Param param;
			param.Name.assign(name);
			param.Type.assign(type);
			out.push_back(std::move(param));
		}

		// 形参表(`(` 与 `)` 之间)→ 逐个参数;顶层逗号切分,`<>`/`()`/`[]`/`{}` 内的逗号不算。
		inline void ParseParams(std::string_view list, std::vector<Param>& out)
		{
			int nesting = 0;
			std::size_t start = 0;
			for (std::size_t index = 0; index <= list.size(); ++index)
			{
				const bool atEnd = index == list.size();
				const char c = atEnd ? ',' : list[index];
				if (!atEnd)
				{
					if (c == '(' || c == '[' || c == '{' || c == '<')
						++nesting;
					else if ((c == ')' || c == ']' || c == '}' || c == '>') && nesting > 0)
						--nesting;
				}
				if (c == ',' && nesting == 0)
				{
					AddParam(list.substr(start, index - start), out);
					start = index + 1;
				}
			}
		}

		// 一行是不是函数 / 方法的签名(`<返回类型> <名字>[<泛型…>](<形参…>)`),
		// 以及它后面是 `{`(定义)还是 `;`(纯声明)。控制语句(`if (`/`for (`/`return (`/`float3(`)
		// 与调用行(`float3 x = Foo(`)都会被返回类型 / 已知符号两处判定挡掉 —— 表里的名字不做函数名。
		inline bool ParseSignature(std::string_view code, const std::vector<std::string>& fileTypes,
			bool nextCodeLineStartsBrace, Signature& out)
		{
			const std::size_t open = code.find('(');
			if (open == std::string_view::npos)
				return false;
			int depth = 0;
			std::size_t close = std::string_view::npos;
			for (std::size_t index = open; index < code.size(); ++index)
			{
				if (code[index] == '(')
					++depth;
				else if (code[index] == ')')
				{
					--depth;
					if (depth == 0)
					{
						close = index;
						break;
					}
				}
			}
			if (close == std::string_view::npos)
				return false;

			// 名字:跳过泛型参数表 `<T : IUVTransform>`(`float2 ApplyUVTransform<T : IUVTransform>(`)。
			std::size_t nameEnd = open;
			while (nameEnd > 0 && IsBlank(code[nameEnd - 1]))
				--nameEnd;
			if (nameEnd > 0 && code[nameEnd - 1] == '>')
			{
				int angle = 0;
				std::size_t probe = nameEnd;
				std::size_t openAngle = std::string_view::npos;
				while (probe > 0)
				{
					--probe;
					if (code[probe] == '>')
						++angle;
					else if (code[probe] == '<')
					{
						--angle;
						if (angle <= 0)
						{
							openAngle = probe;
							break;
						}
					}
				}
				if (openAngle == std::string_view::npos)
					return false;
				nameEnd = openAngle;
				while (nameEnd > 0 && IsBlank(code[nameEnd - 1]))
					--nameEnd;
			}
			std::size_t nameStart = nameEnd;
			while (nameStart > 0 && IsIdentChar(code[nameStart - 1]))
				--nameStart;
			if (nameStart == nameEnd || !IsIdentStart(code[nameStart]))
				return false;
			const std::string_view name = code.substr(nameStart, nameEnd - nameStart);
			// 表里的**关键字 / 类型名**不做函数名:`if (` / `for (` / `return (` / `float3(…)` 这类
			// 形态在这里挡掉。表里的**引擎函数**(`Evaluate` / `MakeDefaultSurface`)反过来必须放行 ——
			// 用户文件的入口就是 `Surface Evaluate(MaterialInputs input)`,漏了它等于没有作用域。
			if (const SlangSymbols::Symbol* known = SlangSymbols::Find(name))
				if (known->Kind == SlangSymbols::Class::Keyword || known->Kind == SlangSymbols::Class::Type)
					return false;

			const std::string_view head = Trim(code.substr(0, nameStart));
			if (head.empty())
				return false;
			std::size_t returnEnd = head.size();
			std::size_t returnStart = returnEnd;
			while (returnStart > 0 && IsIdentChar(head[returnStart - 1]))
				--returnStart;
			const std::string_view returnType = head.substr(returnStart, returnEnd - returnStart);
			if (returnType.empty() || !IsReturnType(returnType, fileTypes))
				return false;

			const std::string_view tail = Trim(code.substr(close + 1));
			const bool braceOnLine = !tail.empty() && tail.front() == '{';
			const bool hasBody = braceOnLine || (tail.empty() && nextCodeLineStartsBrace);
			if (!hasBody && tail != ";")
				return false;

			out.Name.assign(name);
			out.ReturnType.assign(returnType);
			out.Params.clear();
			out.HasBody = hasBody;
			out.BraceOnLine = braceOnLine;
			ParseParams(code.substr(open + 1, close - open - 1), out.Params);
			return true;
		}

		// 语句级声明:`<已知类型> <名字>`(可带 `static`/`const`/`uniform` 等前缀修饰)。
		// 认不出返回 false(不猜);`struct`/`using` 这类引导行由 StartsWithTypeIntro 挡掉。
		inline bool ParseStatementDecl(std::string_view code, const std::vector<std::string>& fileTypes,
			Decl& out)
		{
			std::vector<Word> words;
			Tokenize(code, words);
			if (words.size() < 2)
				return false;
			std::size_t typeIndex = words.size();
			for (std::size_t index = 0; index < words.size(); ++index)
			{
				if (!IsDeclaratorType(words[index].Text, fileTypes))
					continue;
				typeIndex = index;
				break;
			}
			if (typeIndex == words.size() || typeIndex + 1 >= words.size())
				return false;
			for (std::size_t index = 0; index < typeIndex; ++index)
			{
				const SlangSymbols::Symbol* symbol = SlangSymbols::Find(words[index].Text);
				if (symbol == nullptr || symbol->Kind != SlangSymbols::Class::Keyword)
					return false;   // 类型名之前只允许关键字修饰
			}
			const Word& name = words[typeIndex + 1];
			const std::string_view between = code.substr(words[typeIndex].End, name.Start - words[typeIndex].End);
			for (char c : between)
				if (!IsBlank(c) && c != '*' && c != '&' && c != '<' && c != '>' && c != ':')
					return false;
			const std::string_view tail = Trim(code.substr(name.End));
			if (!tail.empty() && tail.front() != '=' && tail.front() != ';' && tail.front() != ','
				&& tail.front() != '[' && tail.front() != '(' && tail.front() != '{')
				return false;
			const std::size_t open = code.find('(');
			if (open != std::string_view::npos && open < name.End)
				return false;   // `for (int i …)` / `if (float3 x)` 这类不索引
			out.Name.assign(name.Text);
			out.Type.assign(words[typeIndex].Text);
			out.Function = false;
			out.TypeName = false;
			return true;
		}

		// 整个缓冲 → 声明表 + 名字集合。只做行级扫描;越界/畸形行一律跳过(不抛异常)。
		inline void ScanDeclarations(std::string_view fileText, FileIndex& out)
		{
			out.Decls.clear();
			out.Scopes.clear();
			out.Names.clear();

			struct LineInfo
			{
				std::string Code;      // 注释/字符串已清空(偏移不变)
				int DepthBefore = 0;
				int DepthAfter = 0;
			};
			std::vector<LineInfo> lines;
			{
				bool blockComment = false;
				int depth = 0;
				std::size_t start = 0;
				for (;;)
				{
					const std::size_t newline = fileText.find('\n', start);
					const std::size_t end = newline == std::string_view::npos ? fileText.size() : newline;
					std::string_view raw = fileText.substr(start, end - start);
					if (!raw.empty() && raw.back() == '\r')
						raw.remove_suffix(1);
					LineInfo info;
					info.DepthBefore = depth;
					info.Code = CleanLine(raw, blockComment);
					for (char c : info.Code)
					{
						if (c == '{')
							++depth;
						else if (c == '}')
							--depth;
					}
					info.DepthAfter = depth;
					lines.push_back(std::move(info));
					if (newline == std::string_view::npos)
						break;
					start = newline + 1;
				}
			}

			const auto enclosingScope = [&out](int line) -> const Scope*
			{
				const Scope* best = nullptr;
				for (const Scope& scope : out.Scopes)
					if (line >= scope.Start && line <= scope.End
						&& (best == nullptr || scope.Start > best->Start))
						best = &scope;
				return best;
			};

			// ① 文件内类型名(struct / interface / class / enum / `enum class`)—— 声明判定和着色都要。
			struct TypeDecl
			{
				std::string Name;
				int Line = 0;
			};
			std::vector<TypeDecl> fileTypes;
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const std::string_view code = Trim(lines[index].Code);
				if (code.empty())
					continue;
				std::size_t introLength = 0;
				for (const char* keyword : { "enum", "struct", "interface", "class" })
				{
					if (MatchesWord(code, keyword))
					{
						introLength = std::string_view(keyword).size();
						break;
					}
				}
				if (introLength == 0)
					continue;
				std::string_view rest = Trim(code.substr(introLength));
				if (MatchesWord(rest, "class"))   // `enum class Foo`
					rest = Trim(rest.substr(5));
				std::size_t end = 0;
				while (end < rest.size() && IsIdentChar(rest[end]))
					++end;
				if (end > 0 && IsIdentStart(rest[0]))
					fileTypes.push_back(TypeDecl { std::string(rest.substr(0, end)),
						static_cast<int>(index) });
			}
			std::vector<std::string> typeNames;
			for (const TypeDecl& type : fileTypes)
				typeNames.push_back(type.Name);

			// ② 函数 / 方法签名 → 函数体行号区间 + 形参;纯声明(`;`)只记名字。
			std::vector<Signature> signatures(lines.size());
			std::vector<int> scopeOfSignature(lines.size(), -1);   // 签名行 → scopes 下标
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				bool nextBrace = false;
				for (std::size_t probe = index + 1; probe < lines.size(); ++probe)
				{
					const std::string_view code = Trim(lines[probe].Code);
					if (code.empty())
						continue;
					nextBrace = code.front() == '{';
					break;
				}
				Signature signature;
				if (!ParseSignature(lines[index].Code, typeNames, nextBrace, signature))
					continue;

				signatures[index] = signature;
				Decl decl;
				decl.Name = signature.Name;
				decl.Type = signature.ReturnType;
				decl.Function = true;
				decl.Line = static_cast<int>(index);
				decl.Origin = lines[index].DepthBefore > 0 ? Origin::Member : Origin::FileScope;
				if (signature.HasBody)
				{
					std::size_t body = index;
					if (!signature.BraceOnLine)
						for (std::size_t probe = index + 1; probe < lines.size(); ++probe)
							if (!Trim(lines[probe].Code).empty())
							{
								body = probe;
								break;
							}
					Scope scope;
					scope.Start = static_cast<int>(body);
					scope.End = static_cast<int>(lines.size()) - 1;
					for (std::size_t probe = body; probe < lines.size(); ++probe)
						if (lines[probe].DepthAfter <= lines[body].DepthBefore)
						{
							scope.End = static_cast<int>(probe);
							break;
						}
					scopeOfSignature[index] = static_cast<int>(out.Scopes.size());
					out.Scopes.push_back(scope);
					decl.ScopeStart = scope.Start;
					decl.ScopeEnd = scope.End;
				}
				out.Decls.push_back(std::move(decl));
			}

			// ③ 文件内类型名 → 声明(着色 + 顶层候选:类型名在文件里哪一行都能用)。
			for (const TypeDecl& type : fileTypes)
			{
				Decl decl;
				decl.Name = type.Name;
				decl.TypeName = true;
				decl.Line = type.Line;
				decl.Origin = lines[static_cast<std::size_t>(type.Line)].DepthBefore > 0
					? Origin::Member : Origin::FileScope;
				out.Decls.push_back(std::move(decl));
			}

			// ④ 逐行:签名行 → 形参;其余行 → 语句级声明(局部 / 文件级 / 成员)。
			for (std::size_t index = 0; index < lines.size(); ++index)
			{
				const LineInfo& line = lines[index];
				const int lineNumber = static_cast<int>(index);
				if (!signatures[index].Name.empty())
				{
					const Signature& signature = signatures[index];
					const int scopeIndex = scopeOfSignature[index];
					if (scopeIndex < 0)
						continue;   // 纯声明:只记名字(上面已加),形参不索引
					const Scope& scope = out.Scopes[static_cast<std::size_t>(scopeIndex)];
					for (const Param& param : signature.Params)
					{
						Decl decl;
						decl.Name = param.Name;
						decl.Type = param.Type;
						decl.Origin = Origin::Parameter;
						decl.Line = lineNumber;
						decl.ScopeStart = scope.Start;
						decl.ScopeEnd = scope.End;
						out.Decls.push_back(std::move(decl));
					}
					continue;
				}
				const std::string_view code = Trim(line.Code);
				if (code.empty() || StartsWithTypeIntro(code))
					continue;
				Decl decl;
				if (!ParseStatementDecl(code, typeNames, decl))
					continue;
				decl.Line = lineNumber;
				if (const Scope* scope = enclosingScope(lineNumber))
				{
					decl.Origin = Origin::Local;
					decl.ScopeStart = scope->Start;
					decl.ScopeEnd = scope->End;
				}
				else
				{
					decl.Origin = line.DepthBefore > 0 ? Origin::Member : Origin::FileScope;
				}
				out.Decls.push_back(std::move(decl));
			}

			for (const Decl& decl : out.Decls)
			{
				bool seen = false;
				for (const std::string& name : out.Names)
					if (name == decl.Name)
					{
						seen = true;
						break;
					}
				if (!seen)
					out.Names.push_back(decl.Name);
			}
		}
	}

	class SlangCompletionIndex
	{
	public:
		// 清空文件符号(词表在 SlangKeywords.h,不在这里)。
		void Clear()
		{
			m_FileParams.clear();
			m_DeclaredNames.clear();
			m_Index.Decls.clear();
			m_Index.Scopes.clear();
			m_Index.Names.clear();
		}

		// 当前文件源码:扫描 `//! param <type> <name> = …`(解析在 SlangKeywords.h 的
		// SlangAnnotations::ParseParamDecl —— 与高亮读同一个实现)+ MAT-INTEL3 的保守语义索引
		// (形参 / 局部变量 / 文件级声明 / 文件内类型名)。容错:少了 `//!` 后的空格(`//!param`)也认;
		// 缺名字的行跳过。重复调用替换上一次结果。
		void SetFileSource(std::string_view fileText)
		{
			SlangAnnotations::ScanParamDecls(fileText, m_FileParams);
			SlangAnnotations::CollectDeclaredNames(fileText, m_DeclaredNames);
			SlangSemantics::ScanDeclarations(fileText, m_Index);
			for (const std::string& name : m_Index.Names)
			{
				bool seen = false;
				for (const std::string& known : m_DeclaredNames)
					if (known == name)
					{
						seen = true;
						break;
					}
				if (!seen)
					m_DeclaredNames.push_back(name);
			}
		}

		// 本文件里已声明的名字(已去重)= `//! param` 声明 + 语义索引(形参 / 局部变量 /
		// 文件级声明 / 类型名 / 成员)。MAT-INTEL4 起**只用于补全/悬停与缓存失效**,不再参与着色。
		const std::vector<std::string>& DeclaredNames() const { return m_DeclaredNames; }

		// 语义索引里的声明数(诊断/自检用;局部/形参/文件级/成员都算)。
		std::size_t DeclarationCount() const { return m_Index.Decls.size(); }

		// 语义索引本体(诊断 / 自检用;口径见 SlangSemantics 的注释)。
		const SlangSemantics::FileIndex& Declarations() const { return m_Index; }

		// 符号总数 = 词表 + 契约字段 + 文件参数 + 语义索引(诊断/自检用,口径与 LuauCompletionIndex 同义)。
		std::size_t SymbolCount() const
		{
			return SlangSymbols::Count() + MaterialInputCount() + SurfaceCount() + m_FileParams.size()
				+ m_Index.Decls.size();
		}

		// 按光标前片段补全(光标行未知 = 只给文件级符号 + 词表)。maxItems = 0 表示不截断。
		void Query(std::string_view linePrefix, std::size_t maxItems,
			std::vector<LuauCompletionItem>& out) const
		{
			QueryAt(linePrefix, -1, maxItems, out);
		}

		// 按光标前片段补全;caretLine = 光标所在行(0 基;<0 = 未知)。局部变量 / 形参只在
		// "同一函数体内、声明行之后"可见,文件级符号总可用(口径见 SlangSemantics 的注释)。
		void QueryAt(std::string_view linePrefix, int caretLine, std::size_t maxItems,
			std::vector<LuauCompletionItem>& out) const
		{
			std::vector<LuauCompletionItem> candidates;
			// parsedPrefix 的寿命必须覆盖 Filter 调用(不要把它塞进内层作用域再取 view)。
			std::string parsedPrefix;
			std::string_view prefix;
			if (SlangAnnotations::IsAnnotationLine(linePrefix))
			{
				prefix = TrailingIdentifier(linePrefix);
				AppendSymbols(candidates, /*annotationContext=*/true);
			}
			else
			{
				std::string receiver;
				char separator = '\0';
				LuauCompletionIndex::ParseContext(linePrefix, receiver, separator, parsedPrefix);
				prefix = parsedPrefix;
				if (separator == '.' && !receiver.empty())
					CollectMembers(candidates, receiver);
				else
					CollectGlobals(candidates, caretLine);
			}
			Filter(candidates, prefix);
			Finish(candidates, maxItems);
			out.swap(candidates);
		}

		// 悬停:同一个词的类型与文档。linePrefix = 词之前的整行片段(含接收者),word = 鼠标下的标识符。
		bool Describe(std::string_view linePrefix, std::string_view word,
			LuauCompletionItem& out) const
		{
			if (word.empty())
				return false;
			if (SlangAnnotations::IsAnnotationLine(linePrefix))
			{
				// 注解行:先查注解词表(param / 类型 / 属性 / [min,max]),再退化到语言符号。
				if (DescribeSymbol(word, /*annotationContext=*/true, out))
					return true;
				return DescribeSymbol(word, /*annotationContext=*/false, out);
			}
			// 接收者判定与 Query 同一条解析:把 word 接回 linePrefix 再解析上下文。
			std::string receiver;
			char separator = '\0';
			std::string prefix;
			std::string joined(linePrefix);
			joined.append(word);
			LuauCompletionIndex::ParseContext(joined, receiver, separator, prefix);
			if (separator == '.' && !receiver.empty())
			{
				std::vector<LuauCompletionItem> members;
				CollectMembers(members, receiver);
				return DescribeItems(members, word, out);
			}
			// MAT-INTEL3:本文件索引里的名字(局部变量 / 形参 / 文件级 / 类型 / 成员)。
			// 悬停是**只读文档**入口,不做作用域过滤(光标行不可知;补全才按作用域过滤)。
			if (DescribeDecl(word, out))
				return true;
			if (DescribeFileParam(word, out))
				return true;
			// 语言关键字 / 类型 / 引擎类型与函数 / 内建 —— 同一张表(高亮读的就是它)。
			return DescribeSymbol(word, /*annotationContext=*/false, out);
		}

	private:
		// 词表条目 = SlangKeywords.h 的唯一表(高亮读同一份)。
		using Symbol = SlangSymbols::Symbol;

		// ---- 过滤 / 排序 / 截断 ----
		static int KindRank(LuauCompletionItem::KindType kind)
		{
			switch (kind)
			{
				case LuauCompletionItem::KindType::Field: return 0;
				case LuauCompletionItem::KindType::Method: return 1;
				case LuauCompletionItem::KindType::Global: return 2;
				case LuauCompletionItem::KindType::Keyword: return 3;
				default: return 4;
			}
		}

		// 前缀零命中 → 子串兜底(与 LuauCompletionIndex 同一口径;`[min,max]` 这类语法片段靠它命中)。
		static void Filter(std::vector<LuauCompletionItem>& items, std::string_view prefix)
		{
			if (prefix.empty())
				return;
			std::vector<LuauCompletionItem> starts;
			for (const LuauCompletionItem& item : items)
				if (SlangText::StartsWithIgnoreCase(item.Name, prefix))
					starts.push_back(item);
			if (!starts.empty())
			{
				items.swap(starts);
				return;
			}
			std::vector<LuauCompletionItem> contains;
			for (const LuauCompletionItem& item : items)
				if (SlangText::ContainsIgnoreCase(item.Name, prefix))
					contains.push_back(item);
			items.swap(contains);
		}

		static void Finish(std::vector<LuauCompletionItem>& items, std::size_t maxItems)
		{
			std::sort(items.begin(), items.end(), [](const LuauCompletionItem& a, const LuauCompletionItem& b)
			{
				const int rankA = KindRank(a.Kind);
				const int rankB = KindRank(b.Kind);
				if (rankA != rankB)
					return rankA < rankB;
				return SlangText::NameLess(a.Name, b.Name);
			});
			if (maxItems > 0 && items.size() > maxItems)
				items.resize(maxItems);
		}

		static std::string_view TrailingIdentifier(std::string_view linePrefix)
		{
			std::size_t end = linePrefix.size();
			while (end > 0 && (linePrefix[end - 1] == ' ' || linePrefix[end - 1] == '\t'))
				--end;
			std::size_t start = end;
			while (start > 0)
			{
				const char c = linePrefix[start - 1];
				const bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
					|| (c >= '0' && c <= '9') || c == '_';
				if (!ident)
					break;
				--start;
			}
			return linePrefix.substr(start, end - start);
		}

		// 词表条目 → 候选(注解属性插入 `group("")` 这类形态,其余用名字本身)。
		static LuauCompletionItem MakeSymbolItem(const Symbol& symbol)
		{
			LuauCompletionItem item;
			item.Name.assign(symbol.Insert.empty() ? symbol.Name : symbol.Insert);
			item.Type.assign(symbol.Category);
			item.Doc.assign(symbol.Doc);
			item.Kind = KindFor(symbol.Kind);
			return item;
		}

		// 词表分类 → 补全 Kind(排序档位由它决定:Field → Method → Global → Keyword)。
		// MAT-INTEL4:浮层只画"名字 + Type 列 + 文档",**没有 kind 列**,所以这里不改引擎结构:
		// 类别语义与高亮一致(类型/关键字 → Keyword 档、函数 → Method 档、字段/注解键 → Field 档),
		// `Type` 列文字仍是表里的 Category(引擎侧已把该列改成类型色)。
		static LuauCompletionItem::KindType KindFor(SlangSymbols::Class kind)
		{
			switch (kind)
			{
				case SlangSymbols::Class::Keyword:
				case SlangSymbols::Class::Type:
				case SlangSymbols::Class::EngineType:
				case SlangSymbols::Class::AnnotationType:
					return LuauCompletionItem::KindType::Keyword;
				case SlangSymbols::Class::AnnotationKey:
				case SlangSymbols::Class::AnnotationSyntax:
					return LuauCompletionItem::KindType::Field;
				default: // Intrinsic / EngineFunction / AnnotationAttr
					return LuauCompletionItem::KindType::Method;
			}
		}

		// 这个词在这一上下文里算不算候选(高亮不看上下文,补全要看:注解词不进代码候选池)。
		static bool SymbolInContext(const Symbol& symbol, bool annotationContext)
		{
			if (symbol.Where == SlangSymbols::Context::Both)
				return true;
			return annotationContext ? symbol.Where == SlangSymbols::Context::Annotation
				: symbol.Where == SlangSymbols::Context::Code;
		}

		// skipExisting = true 时,名字已经在候选里(来自语义索引 / 文件参数)的词表条目跳过 ——
		// 同一个名字在浮层里只出现一次,索引里的那条(带"local/parameter/file-scope"文档)优先。
		static void AppendSymbols(std::vector<LuauCompletionItem>& out, bool annotationContext,
			bool skipExisting = false)
		{
			for (std::size_t i = 0; i < SlangSymbols::Count(); ++i)
			{
				const Symbol& symbol = SlangSymbols::kSymbols[i];
				if (!SymbolInContext(symbol, annotationContext))
					continue;
				if (skipExisting)
				{
					bool seen = false;
					for (const LuauCompletionItem& existing : out)
						if (existing.Name == symbol.Name)
						{
							seen = true;
							break;
						}
					if (seen)
						continue;
				}
				out.push_back(MakeSymbolItem(symbol));
			}
		}

		static bool DescribeSymbol(std::string_view word, bool annotationContext, LuauCompletionItem& out)
		{
			const Symbol* symbol = SlangSymbols::Find(word);
			if (symbol == nullptr || !SymbolInContext(*symbol, annotationContext))
				return false;
			out = MakeSymbolItem(*symbol);
			return true;
		}

		static bool DescribeItems(const std::vector<LuauCompletionItem>& items, std::string_view word,
			LuauCompletionItem& out)
		{
			for (const LuauCompletionItem& item : items)
			{
				if (item.Name != word)
					continue;
				out = item;
				return true;
			}
			return false;
		}

		bool DescribeFileParam(std::string_view word, LuauCompletionItem& out) const
		{
			for (const SlangAnnotations::ParamDecl& param : m_FileParams)
			{
				if (param.Name != word)
					continue;
				out = MakeFileParamItem(param);
				return true;
			}
			return false;
		}

		static LuauCompletionItem MakeFileParamItem(const SlangAnnotations::ParamDecl& param)
		{
			LuauCompletionItem item;
			item.Name = param.Name;
			item.Type = param.Type;
			item.Doc = "declared in this file: //! param " + param.Type + " " + param.Name
				+ (param.Default.empty() ? std::string() : " = " + param.Default);
			item.Kind = LuauCompletionItem::KindType::Global;
			return item;
		}

		// 参数注解的解析 `//! param …` 与高亮共用(见 SlangKeywords.h 的
		// SlangAnnotations::ParseParamDecl / ScanParamDecls —— 本类不再自己解析一份),
		// 关键字 / 内建 / 引擎类型与函数 / 注解词同样只从 SlangSymbols 的唯一表取
		// (旧版本的 EngineHelperTable / TypeTable / BuiltinTable / KeywordTable / AnnotationTable
		//  已删除:它们高亮一份、补全另一份,正是"高亮得出、补全不出来"的根因)。

		// ---- 候选收集 ----
		void CollectMembers(std::vector<LuauCompletionItem>& out, std::string_view receiver) const
		{
			if (receiver == "input")
				AppendContractFields(out, MaterialSurfaceContract::MaterialInputFields,
					MaterialInputCount(), false);
			else if (receiver == "surface")
				AppendContractFields(out, MaterialSurfaceContract::SurfaceFields,
					SurfaceCount(), true);
			// 其它接收者(Slang 对象成员)不在本表覆盖范围内:给空候选,不猜。
		}

		// 代码上下文 = 语义索引(局部 / 形参 / 文件级)+ 文件参数 + 唯一词表。
		// 顺序(与 Finish 的档位一致):索引里的局部/形参(Field)→ 文件级(Global)→ 词表(Keyword);
		// **同名去重**:索引里的名字赢(例如局部 `input` 不再被词表的引擎契约条目顶掉)。
		void CollectGlobals(std::vector<LuauCompletionItem>& out, int caretLine) const
		{
			CollectIndexed(out, caretLine);
			for (const SlangAnnotations::ParamDecl& param : m_FileParams)
				PushUnique(out, MakeFileParamItem(param));
			AppendSymbols(out, /*annotationContext=*/false, /*skipExisting=*/true);
		}

		// 光标所在行可见的索引符号:文件级总可用;局部/形参只在同一函数体内、声明行之后可用
		// (跨函数不可见);结构体字段/方法不进顶层候选(MAT-INTEL4 起名字也不着色)。
		void CollectIndexed(std::vector<LuauCompletionItem>& out, int caretLine) const
		{
			const int scope = CaretScope(caretLine);
			for (const SlangSemantics::Decl& decl : m_Index.Decls)
			{
				if (decl.Origin == SlangSemantics::Origin::Member)
					continue;
				if (decl.Origin != SlangSemantics::Origin::FileScope)
				{
					if (scope < 0 || decl.ScopeStart != scope)
						continue;
					if (decl.Line > caretLine)
						continue;   // 声明在光标之后 → 还不可见
				}
				PushUnique(out, MakeDeclItem(decl));
			}
		}

		// 光标行所属函数体的 `{` 行;不属于任何函数体(文件级 / 未知)返回 -1。
		int CaretScope(int caretLine) const
		{
			if (caretLine < 0)
				return -1;
			int best = -1;
			for (const SlangSemantics::Scope& scope : m_Index.Scopes)
				if (caretLine >= scope.Start && caretLine <= scope.End && scope.Start > best)
					best = scope.Start;
			return best;
		}

		static void PushUnique(std::vector<LuauCompletionItem>& out, LuauCompletionItem item)
		{
			for (const LuauCompletionItem& existing : out)
				if (existing.Name == item.Name)
					return;
			out.push_back(std::move(item));
		}

		bool DescribeDecl(std::string_view word, LuauCompletionItem& out) const
		{
			for (const SlangSemantics::Decl& decl : m_Index.Decls)
			{
				if (decl.Name != word)
					continue;
				out = MakeDeclItem(decl);
				return true;
			}
			return false;
		}

		// 索引条目 → 候选(局部/形参 Kind=Field 排最前;文件级 Kind=Global 排在关键字之前)。
		static LuauCompletionItem MakeDeclItem(const SlangSemantics::Decl& decl)
		{
			LuauCompletionItem item;
			item.Name = decl.Name;
			item.Type = decl.TypeName ? std::string("type") : decl.Type;
			if (item.Type.empty())
				item.Type = "unknown";
			item.Doc = DescribeDeclText(decl);
			item.Kind = decl.Origin == SlangSemantics::Origin::FileScope
				? LuauCompletionItem::KindType::Global : LuauCompletionItem::KindType::Field;
			return item;
		}

		// `local` / `parameter` / `file-scope` 三个词是报告与探针的判据,不要改成同义词。
		static std::string DescribeDeclText(const SlangSemantics::Decl& decl)
		{
			const std::string line = std::to_string(decl.Line + 1);
			if (decl.TypeName)
				return (decl.Origin == SlangSemantics::Origin::Member ? std::string("member type")
					: std::string("file-scope type")) + " (declared on line " + line + ")";
			if (decl.Function)
				return (decl.Origin == SlangSemantics::Origin::Member ? std::string("member function")
					: std::string("file-scope function")) + " returning " + decl.Type
					+ " (declared on line " + line + ")";
			switch (decl.Origin)
			{
				case SlangSemantics::Origin::Parameter:
					return "parameter " + decl.Type + " (declared on line " + line + ")";
				case SlangSemantics::Origin::Local:
					return "local " + decl.Type + " (declared on line " + line + ")";
				case SlangSemantics::Origin::Member:
					return "member " + decl.Type + " (declared on line " + line + ")";
				case SlangSemantics::Origin::FileScope:
				default:
					return "file-scope " + decl.Type + " (declared on line " + line + ")";
			}
		}

		void AppendContractFields(std::vector<LuauCompletionItem>& out,
			const MaterialSurfaceContract::FieldInfo* fields, std::size_t count, bool withDefault) const
		{
			for (std::size_t i = 0; i < count; ++i)
			{
				const MaterialSurfaceContract::FieldInfo& field = fields[i];
				LuauCompletionItem item;
				item.Name = field.Name;
				item.Type = field.HlslType;
				item.Doc = field.Semantic;
				if (withDefault && field.DefaultExpression != nullptr && field.DefaultExpression[0] != '\0')
					item.Doc += std::string(" — default ") + field.DefaultExpression;
				else if (!withDefault)
					item.Doc += " (MaterialInputs)";
				item.Kind = LuauCompletionItem::KindType::Field;
				out.push_back(std::move(item));
			}
		}

		static constexpr std::size_t MaterialInputCount()
		{
			return sizeof(MaterialSurfaceContract::MaterialInputFields)
				/ sizeof(MaterialSurfaceContract::MaterialInputFields[0]);
		}
		static constexpr std::size_t SurfaceCount()
		{
			return sizeof(MaterialSurfaceContract::SurfaceFields)
				/ sizeof(MaterialSurfaceContract::SurfaceFields[0]);
		}

		std::vector<SlangAnnotations::ParamDecl> m_FileParams;
		// 给高亮用的名字(`//! param` 声明 + 语义索引,已去重;着色不看作用域)。
		std::vector<std::string> m_DeclaredNames;
		// MAT-INTEL3:保守语义索引(形参 / 局部变量 / 文件级声明 / 文件内类型名 / 成员)。
		SlangSemantics::FileIndex m_Index;
	};
}
