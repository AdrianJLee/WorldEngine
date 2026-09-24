#pragma once

// MAT-INTEL(用户 2026-09-24:「接下来做材质编辑器代码的智能提示,格式化以及完善知识库和资料」):
// 材质着色器(`.slang`)的补全 + 悬停文档表 —— 编辑器侧 header-only(与 SlangHighlight.h 同一口径)。
//
// 复用 Wui::CodeEditor 已经具备的补全浮层 / Hover 基础设施(见 WuiCodeEditor.h 的
// `Completion` / `Hover` / `CompletionIdPrefix`):候选与文档用引擎冻结结构 `LuauCompletionItem`
// (Name/Type/Doc/Params/Kind),**不新增引擎接口**,也不改 WuiCodeEditor / LuauCompletionIndex。
//
// 事实源(不要在别处再手写一份):
//   - `MaterialInputs` / `Surface` 的字段名、类型、语义、默认表达式 =
//     `World/Renderer/MaterialSurfaceContract.hlsli` 的 X-macro(MaterialEditorPanel.h 已经
//     通过 World/Renderer/MaterialSurface.h 看到它;这里直接读 `MaterialSurfaceContract::*Fields`);
//   - 注解语法 = `docs/dev/shader-contract.md` §6;引擎函数名同该文档 §2/§4 与包装模板;
//   - 本文件声明的 `//! param` 名字来自当前缓冲(SetFileSource)。
//
// 查询口径(与 LuauCompletionIndex 一致,让两个编辑器的候选行为对得上):
//   Query(linePrefix) = 上下文(接收者 / 分隔符 / 前缀)→ 候选(成员 | 文件符号+引擎函数+类型+内建+关键字 |
//   注解)→ 过滤(前缀大小写不敏感;前缀零命中时退化为子串匹配)→ 排序(Field→Method→Global→Keyword→Class,
//   同档按名字大小写不敏感升序)→ maxItems 截断。
//   Describe(linePrefix, word) = 同一个词的类型与文档(输入/表面字段、文件参数、引擎函数、类型/内建),
//   Hover 与补全共用这一份数据。

#include "SlangHighlight.h"

#include "World/Renderer/MaterialSurfaceContract.hlsli"
#include "World/Script/LuauCompletion.h"
#include "World/WUI/WuiCodeEditor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace World
{
	// ---- `//!` 注解行(补全闸门的关键:见 HighlightLineWithAnnotations 的说明)----
	namespace SlangAnnotations
	{
		inline bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r';
		}

		// 行首(允许前置空白)是不是 `//!` 注解行。
		inline bool IsAnnotationLine(std::string_view line)
		{
			std::size_t index = 0;
			while (index < line.size() && IsSpace(line[index]))
				++index;
			if (line.size() - index < 3)
				return false;
			return line[index] == '/' && line[index + 1] == '/' && line[index + 2] == '!';
		}

		// 注解体起点 = 前导空白 + `//!` + 一个可选空格;不是注解行返回 0。
		inline std::size_t BodyStart(std::string_view line)
		{
			if (!IsAnnotationLine(line))
				return 0;
			std::size_t index = 0;
			while (index < line.size() && IsSpace(line[index]))
				++index;
			index += 3;
			if (index < line.size() && line[index] == ' ')
				++index;
			return index;
		}

		// 逐行 token(注解行专用):前导空白 + `//!`(含其后的一个空格)是 Comment,注解体按
		// Slang 语法着色(偏移平移到整行),行尾状态交给下一行。
		//
		// 为什么必须分开着色:WuiCodeEditor 只在"光标不在 String/Comment token 里"时查询补全
		// provider(见 WuiCodeEditor.cpp 的 caretInStringOrComment)。注解体若整行 Comment,
		// `//! param …` 上永远弹不出补全;分开后注解体里的光标落在普通 token/空隙上,补全恢复,
		// 而 `label("…")` 这类字符串仍是 String 色 —— 字符串里照旧不弹(与代码区一致)。
		inline void HighlightLineWithAnnotations(std::string_view line, SlangHighlightState& state,
			std::vector<Wui::WuiCodeToken>& out)
		{
			out.clear();
			if (!IsAnnotationLine(line))
			{
				SlangHighlighter::HighlightLine(line, state, out);
				return;
			}
			const std::size_t body = BodyStart(line);
			if (body > 0)
			{
				out.push_back(Wui::WuiCodeToken { 0, static_cast<uint32_t>(body),
					Wui::WuiCodeTokenKind::Comment });
			}
			// 注解行不可能接在块注释里(行首就是 `//!`),所以注解体从零状态起。
			SlangHighlightState bodyState;
			std::vector<Wui::WuiCodeToken> bodyTokens;
			SlangHighlighter::HighlightLine(line.substr(body), bodyState, bodyTokens);
			for (const Wui::WuiCodeToken& token : bodyTokens)
			{
				out.push_back(Wui::WuiCodeToken {
					token.StartByte + static_cast<uint32_t>(body),
					token.EndByte + static_cast<uint32_t>(body), token.Kind });
			}
			state = bodyState;
		}
	}

	class SlangCompletionIndex
	{
	public:
		// 清空文件符号(字段/引擎函数/类型/内建/关键字/注解是内置表,不在这里)。
		void Clear() { m_FileParams.clear(); }

		// 当前文件源码:扫描 `//! param <type> <name> = …` 的名字与类型,作为顶层候选与悬停文档。
		// 容错:少了 `//!` 后的空格(`//!param`)也认;缺名字的行跳过。重复调用替换上一次结果。
		void SetFileSource(std::string_view fileText)
		{
			m_FileParams.clear();
			std::size_t start = 0;
			for (;;)
			{
				const std::size_t end = fileText.find('\n', start);
				const std::size_t lineEnd = end == std::string_view::npos ? fileText.size() : end;
				std::string_view line = fileText.substr(start, lineEnd - start);
				if (!line.empty() && line.back() == '\r')
					line.remove_suffix(1);
				FileParam param;
				if (ParseParamAnnotation(line, param))
					m_FileParams.push_back(std::move(param));
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
		}

		// 符号总数 = 内置表 + 文件参数(诊断/自检用,口径与 LuauCompletionIndex 同义)。
		std::size_t SymbolCount() const
		{
			return MaterialInputCount() + SurfaceCount() + EngineHelperCount() + TypeCount()
				+ BuiltinCount() + KeywordCount() + AnnotationCount() + m_FileParams.size();
		}

		// 按光标前片段补全。maxItems = 0 表示不截断。
		void Query(std::string_view linePrefix, std::size_t maxItems,
			std::vector<LuauCompletionItem>& out) const
		{
			std::vector<LuauCompletionItem> candidates;
			// parsedPrefix 的寿命必须覆盖 Filter 调用(不要把它塞进内层作用域再取 view)。
			std::string parsedPrefix;
			std::string_view prefix;
			if (SlangAnnotations::IsAnnotationLine(linePrefix))
			{
				prefix = TrailingIdentifier(linePrefix);
				AppendTable(candidates, AnnotationTable(), AnnotationCount());
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
					CollectGlobals(candidates);
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
				return DescribeTable(AnnotationTable(), AnnotationCount(), word, out);
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
			if (DescribeFileParam(word, out))
				return true;
			if (DescribeTable(EngineHelperTable(), EngineHelperCount(), word, out))
				return true;
			if (DescribeTable(TypeTable(), TypeCount(), word, out))
				return true;
			if (DescribeTable(BuiltinTable(), BuiltinCount(), word, out))
				return true;
			return DescribeTable(KeywordTable(), KeywordCount(), word, out);
		}

	private:
		// 一条内置候选(Name/Type/Doc 都是静态字面量,生命周期 = 程序)。
		struct Entry
		{
			std::string_view Name;
			std::string_view Type;
			std::string_view Doc;
			LuauCompletionItem::KindType Kind;
		};

		struct FileParam
		{
			std::string Name;
			std::string Type;
			std::string Default;
		};

		// ---- 过滤 / 排序 / 截断 ----
		static char LowerAscii(char c)
		{
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		static bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
		{
			if (prefix.size() > text.size())
				return false;
			for (std::size_t i = 0; i < prefix.size(); ++i)
				if (LowerAscii(text[i]) != LowerAscii(prefix[i]))
					return false;
			return true;
		}

		static bool ContainsIgnoreCase(std::string_view text, std::string_view needle)
		{
			if (needle.empty())
				return true;
			if (needle.size() > text.size())
				return false;
			for (std::size_t start = 0; start + needle.size() <= text.size(); ++start)
				if (StartsWithIgnoreCase(text.substr(start), needle))
					return true;
			return false;
		}

		// 大小写不敏感的名字比较(同档内的排序口径;严格弱序)。
		static bool NameLess(std::string_view a, std::string_view b)
		{
			const std::size_t shared = std::min(a.size(), b.size());
			for (std::size_t i = 0; i < shared; ++i)
			{
				const char left = LowerAscii(a[i]);
				const char right = LowerAscii(b[i]);
				if (left != right)
					return left < right;
			}
			return a.size() < b.size();
		}

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
				if (StartsWithIgnoreCase(item.Name, prefix))
					starts.push_back(item);
			if (!starts.empty())
			{
				items.swap(starts);
				return;
			}
			std::vector<LuauCompletionItem> contains;
			for (const LuauCompletionItem& item : items)
				if (ContainsIgnoreCase(item.Name, prefix))
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
				return NameLess(a.Name, b.Name);
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

		static LuauCompletionItem MakeItem(const Entry& entry)
		{
			LuauCompletionItem item;
			item.Name.assign(entry.Name);
			item.Type.assign(entry.Type);
			item.Doc.assign(entry.Doc);
			item.Kind = entry.Kind;
			return item;
		}

		static void AppendTable(std::vector<LuauCompletionItem>& out, const Entry* table, std::size_t count)
		{
			for (std::size_t i = 0; i < count; ++i)
				out.push_back(MakeItem(table[i]));
		}

		static bool DescribeTable(const Entry* table, std::size_t count, std::string_view word,
			LuauCompletionItem& out)
		{
			for (std::size_t i = 0; i < count; ++i)
			{
				if (table[i].Name != word)
					continue;
				out = MakeItem(table[i]);
				return true;
			}
			return false;
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
			for (const FileParam& param : m_FileParams)
			{
				if (param.Name != word)
					continue;
				out = MakeFileParamItem(param);
				return true;
			}
			return false;
		}

		static LuauCompletionItem MakeFileParamItem(const FileParam& param)
		{
			LuauCompletionItem item;
			item.Name = param.Name;
			item.Type = param.Type;
			item.Doc = "declared in this file: //! param " + param.Type + " " + param.Name
				+ (param.Default.empty() ? std::string() : " = " + param.Default);
			item.Kind = LuauCompletionItem::KindType::Global;
			return item;
		}

		// `//! param <type> <name> = <default> …` → 名字 / 类型 / 默认值(容错:缺空格也认)。
		// 默认值取到 `[` 或 group(/label(/unit( 之前的文本(注解里这几项互不嵌套)。
		static bool ParseParamAnnotation(std::string_view line, FileParam& out)
		{
			if (!SlangAnnotations::IsAnnotationLine(line))
				return false;
			const std::string_view body = line.substr(SlangAnnotations::BodyStart(line));
			if (body.size() < 5 || !StartsWithIgnoreCase(body, "param"))
				return false;
			std::size_t index = 5;
			auto skipSpace = [&]()
			{
				while (index < body.size() && (body[index] == ' ' || body[index] == '\t'))
					++index;
			};
			auto readWord = [&]() -> std::string_view
			{
				const std::size_t start = index;
				while (index < body.size())
				{
					const char c = body[index];
					const bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
						|| (c >= '0' && c <= '9') || c == '_';
					if (!ident)
						break;
					++index;
				}
				return body.substr(start, index - start);
			};
			skipSpace();
			const std::string_view type = readWord();
			skipSpace();
			const std::string_view name = readWord();
			if (type.empty() || name.empty())
				return false;
			out.Type.assign(type);
			out.Name.assign(name);
			out.Default.clear();
			skipSpace();
			if (index >= body.size() || body[index] != '=')
				return true;
			++index;
			while (index < body.size() && (body[index] == ' ' || body[index] == '\t'))
				++index;
			std::size_t end = body.size();
			for (std::size_t probe = index; probe < body.size(); ++probe)
			{
				const bool attribute = body[probe] == '['
					|| (body[probe] == ' ' && (StartsWithIgnoreCase(body.substr(probe + 1), "group(")
						|| StartsWithIgnoreCase(body.substr(probe + 1), "label(")
						|| StartsWithIgnoreCase(body.substr(probe + 1), "unit(")));
				if (attribute)
				{
					end = probe;
					break;
				}
			}
			while (end > index && (body[end - 1] == ' ' || body[end - 1] == '\t'))
				--end;
			out.Default.assign(body.substr(index, end - index));
			return true;
		}

		// ---- 内置表 ----
		// 引擎包装层提供的函数(名字与 docs/dev/shader-contract.md §2/§4、包装模板一致)。
		static const Entry* EngineHelperTable()
		{
			static const Entry kTable[] = {
				{ "MakeDefaultSurface", "Surface",
					"Engine default surface. Take it first, then set only the fields this material needs.",
					LuauCompletionItem::KindType::Method },
				{ "WeLinearizeColor", "float3(float3)",
					"Convert an sRGB colour to linear RGB. Surface colours are linear; display encoding is handled by the engine.",
					LuauCompletionItem::KindType::Method },
				{ "WeDefaultNormal", "float3",
					"Default tangent-space normal (0,0,1) — keeps the normal the engine sampled.",
					LuauCompletionItem::KindType::Method },
				{ "WeIdentityMatrix", "float4x4",
					"Identity matrix helper from the wrapper (skinning fallback).",
					LuauCompletionItem::KindType::Method },
				{ "Evaluate", "Surface(MaterialInputs)",
					"The entry point the engine wraps. Signature is fixed: Surface Evaluate(MaterialInputs input).",
					LuauCompletionItem::KindType::Method },
			};
			return kTable;
		}
		static constexpr std::size_t EngineHelperCount() { return 5; }

		// Slang 标量/向量类型(严格子集里会真的用到的那些;与 SlangHighlighter 的关键字表同源)。
		static const Entry* TypeTable()
		{
			static const Entry kTable[] = {
				{ "float", "scalar", "32-bit float scalar. Slang does not convert widths implicitly.",
					LuauCompletionItem::KindType::Keyword },
				{ "float2", "vector", "2-component float vector. Construct explicitly: float2(a, b).",
					LuauCompletionItem::KindType::Keyword },
				{ "float3", "vector", "3-component float vector. Truncate with .xyz/.rgb — implicit width conversion is an error.",
					LuauCompletionItem::KindType::Keyword },
				{ "float4", "vector", "4-component float vector (colour literals are usually float4: r, g, b, a).",
					LuauCompletionItem::KindType::Keyword },
				{ "float3x3", "matrix", "3x3 float matrix.", LuauCompletionItem::KindType::Keyword },
				{ "float4x4", "matrix", "4x4 float matrix (skin/bone transforms).",
					LuauCompletionItem::KindType::Keyword },
				{ "half", "scalar", "16-bit float scalar (no implicit conversions).",
					LuauCompletionItem::KindType::Keyword },
				{ "double", "scalar", "64-bit float scalar (no implicit narrowing to float).",
					LuauCompletionItem::KindType::Keyword },
				{ "int", "scalar", "32-bit signed integer scalar.", LuauCompletionItem::KindType::Keyword },
				{ "uint", "scalar", "32-bit unsigned integer scalar.", LuauCompletionItem::KindType::Keyword },
				{ "bool", "scalar", "Boolean scalar (true / false).", LuauCompletionItem::KindType::Keyword },
				{ "MaterialInputs", "struct",
					"Per-pixel inputs handed to Evaluate: input.UV / input.WorldPosition / input.WorldNormal / input.WorldTangent / input.WorldBitangent (contract §2).",
					LuauCompletionItem::KindType::Keyword },
				{ "Surface", "struct",
					"What Evaluate returns: surface.BaseColor / Metallic / Roughness / Emissive / Normal / Opacity / AmbientOcclusion (contract §2).",
					LuauCompletionItem::KindType::Keyword },
			};
			return kTable;
		}
		static constexpr std::size_t TypeCount() { return 13; }

		// Slang 内建函数(与 SlangHighlight.h 的 IsIntrinsic 同一族;Type 是参数占位,插入时自动补 `()`)。
		static const Entry* BuiltinTable()
		{
			static const Entry kTable[] = {
				{ "abs", "x", "Absolute value.", LuauCompletionItem::KindType::Method },
				{ "acos", "x", "Arc cosine (radians).", LuauCompletionItem::KindType::Method },
				{ "asin", "x", "Arc sine (radians).", LuauCompletionItem::KindType::Method },
				{ "atan", "x", "Arc tangent (radians).", LuauCompletionItem::KindType::Method },
				{ "atan2", "y, x", "Four-quadrant arc tangent (radians).", LuauCompletionItem::KindType::Method },
				{ "ceil", "x", "Round up to the nearest integer value.", LuauCompletionItem::KindType::Method },
				{ "clamp", "x, min, max", "Clamp x into [min, max].", LuauCompletionItem::KindType::Method },
				{ "cos", "x", "Cosine (radians).", LuauCompletionItem::KindType::Method },
				{ "cross", "a, b", "Cross product of two float3 vectors.", LuauCompletionItem::KindType::Method },
				{ "degrees", "x", "Radians to degrees.", LuauCompletionItem::KindType::Method },
				{ "distance", "a, b", "Distance between two points.", LuauCompletionItem::KindType::Method },
				{ "dot", "a, b", "Dot product.", LuauCompletionItem::KindType::Method },
				{ "exp", "x", "e^x.", LuauCompletionItem::KindType::Method },
				{ "exp2", "x", "2^x.", LuauCompletionItem::KindType::Method },
				{ "floor", "x", "Round down to the nearest integer value.", LuauCompletionItem::KindType::Method },
				{ "frac", "x", "Fractional part.", LuauCompletionItem::KindType::Method },
				{ "length", "v", "Vector length.", LuauCompletionItem::KindType::Method },
				{ "lerp", "a, b, t", "Linear interpolation: a + (b - a) * t.", LuauCompletionItem::KindType::Method },
				{ "log", "x", "Natural logarithm.", LuauCompletionItem::KindType::Method },
				{ "log2", "x", "Base-2 logarithm.", LuauCompletionItem::KindType::Method },
				{ "mad", "a, b, c", "a * b + c.", LuauCompletionItem::KindType::Method },
				{ "max", "a, b", "Component-wise maximum.", LuauCompletionItem::KindType::Method },
				{ "min", "a, b", "Component-wise minimum.", LuauCompletionItem::KindType::Method },
				{ "mul", "a, b", "Matrix/vector multiply.", LuauCompletionItem::KindType::Method },
				{ "normalize", "v", "Unit vector (zero-length input stays zero).", LuauCompletionItem::KindType::Method },
				{ "pow", "x, y", "x^y. Wrap negative bases in saturate() yourself.", LuauCompletionItem::KindType::Method },
				{ "radians", "x", "Degrees to radians.", LuauCompletionItem::KindType::Method },
				{ "reflect", "i, n", "Reflect vector i about normal n.", LuauCompletionItem::KindType::Method },
				{ "refract", "i, n, eta", "Refract vector i through normal n.", LuauCompletionItem::KindType::Method },
				{ "round", "x", "Round to the nearest integer value.", LuauCompletionItem::KindType::Method },
				{ "rsqrt", "x", "1 / sqrt(x).", LuauCompletionItem::KindType::Method },
				{ "saturate", "x", "Clamp to [0,1] (works on scalars and vectors).", LuauCompletionItem::KindType::Method },
				{ "sign", "x", "-1, 0 or 1.", LuauCompletionItem::KindType::Method },
				{ "sin", "x", "Sine (radians).", LuauCompletionItem::KindType::Method },
				{ "smoothstep", "min, max, x", "Smooth Hermite interpolation between min and max.", LuauCompletionItem::KindType::Method },
				{ "sqrt", "x", "Square root.", LuauCompletionItem::KindType::Method },
				{ "step", "edge, x", "0 when x < edge, 1 otherwise.", LuauCompletionItem::KindType::Method },
				{ "tan", "x", "Tangent (radians).", LuauCompletionItem::KindType::Method },
				{ "transpose", "m", "Matrix transpose.", LuauCompletionItem::KindType::Method },
			};
			return kTable;
		}
		static constexpr std::size_t BuiltinCount() { return 39; }

		// Slang/HLSL 关键字与字面量(与 SlangHighlight.h 的关键字表同一族)。
		static const Entry* KeywordTable()
		{
			static const Entry kTable[] = {
				{ "static", "keyword", "File-scope static declaration.", LuauCompletionItem::KindType::Keyword },
				{ "const", "keyword", "Read-only value (usual for helper constants).", LuauCompletionItem::KindType::Keyword },
				{ "struct", "keyword", "Struct definition (helper data for Evaluate).", LuauCompletionItem::KindType::Keyword },
				{ "if", "keyword", "Conditional branch — parameter-driven shading lives here.", LuauCompletionItem::KindType::Keyword },
				{ "else", "keyword", "Alternative branch.", LuauCompletionItem::KindType::Keyword },
				{ "for", "keyword", "Loop (keep it bounded: this runs per pixel).", LuauCompletionItem::KindType::Keyword },
				{ "while", "keyword", "Loop with a condition.", LuauCompletionItem::KindType::Keyword },
				{ "return", "keyword", "Return the Surface from Evaluate.", LuauCompletionItem::KindType::Keyword },
				{ "discard", "keyword", "Discard the fragment (alpha-cutout style effects).", LuauCompletionItem::KindType::Keyword },
				{ "true", "literal", "Boolean true.", LuauCompletionItem::KindType::Keyword },
				{ "false", "literal", "Boolean false.", LuauCompletionItem::KindType::Keyword },
			};
			return kTable;
		}
		static constexpr std::size_t KeywordCount() { return 11; }

		// `//!` 注解行候选(语法见 docs/dev/shader-contract.md §6)。
		// group/label/unit 写成 `名字("")` 形态:接受后插入 `group("")` 并选中那段字符串(引擎 W9.8
		// 的片段导航会选中第一个参数)。
		static const Entry* AnnotationTable()
		{
			static const Entry kTable[] = {
				{ "param", "annotation",
					"//! param <type> <name> = <default> [min,max] unit(\"\") group(\"\") label(\"\") — the annotation is the single source of truth for editor rows, cbuffer layout and slots.",
					LuauCompletionItem::KindType::Field },
				{ "group(\"\")", "\"Group\"",
					"group(\"Appearance\") — row group in the material editor (empty = Parameters).",
					LuauCompletionItem::KindType::Method },
				{ "label(\"\")", "\"Label\"",
					"label(\"Tint\") — display name for the row (empty = the parameter name).",
					LuauCompletionItem::KindType::Method },
				{ "unit(\"\")", "\"%\"", "unit(\"%\") — unit suffix shown next to the value.",
					LuauCompletionItem::KindType::Method },
				{ "[min,max]", "range",
					"[0,1] — value range; Float/Int only, default [0,1], the default must fall inside.",
					LuauCompletionItem::KindType::Field },
				{ "Float", "annotation type", "Float — number; [min,max] applies.",
					LuauCompletionItem::KindType::Keyword },
				{ "Int", "annotation type", "Int — integer; [min,max] applies.",
					LuauCompletionItem::KindType::Keyword },
				{ "Bool", "annotation type", "Bool — true / false.", LuauCompletionItem::KindType::Keyword },
				{ "Vec2", "annotation type", "Vec2 — two comma-separated literals (no [min,max]).",
					LuauCompletionItem::KindType::Keyword },
				{ "Vec3", "annotation type", "Vec3 — three comma-separated literals (no [min,max]).",
					LuauCompletionItem::KindType::Keyword },
				{ "Vec4", "annotation type", "Vec4 — four comma-separated literals (no [min,max]).",
					LuauCompletionItem::KindType::Keyword },
				{ "Color", "annotation type", "Color — r, g, b, a literals (linear; no [min,max]).",
					LuauCompletionItem::KindType::Keyword },
				{ "Texture2D", "annotation type",
					"Texture2D — content-root relative path (\"\" = white 1×1 fallback). Slots t4..t11 by annotation order, max 8.",
					LuauCompletionItem::KindType::Keyword },
			};
			return kTable;
		}
		static constexpr std::size_t AnnotationCount() { return 13; }

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

		void CollectGlobals(std::vector<LuauCompletionItem>& out) const
		{
			for (const FileParam& param : m_FileParams)
				out.push_back(MakeFileParamItem(param));
			AppendTable(out, EngineHelperTable(), EngineHelperCount());
			AppendTable(out, TypeTable(), TypeCount());
			AppendTable(out, BuiltinTable(), BuiltinCount());
			AppendTable(out, KeywordTable(), KeywordCount());
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

		std::vector<FileParam> m_FileParams;
	};
}
