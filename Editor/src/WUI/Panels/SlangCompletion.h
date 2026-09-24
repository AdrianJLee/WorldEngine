#pragma once

// MAT-INTEL(用户 2026-09-24:「接下来做材质编辑器代码的智能提示,格式化以及完善知识库和资料」):
// 材质着色器(`.slang`)的补全 + 悬停文档表 —— 编辑器侧 header-only(与 SlangHighlight.h 同一口径)。
// MAT-INTEL2(用户 2026-09-24:「interface 这种关键字没有提示」):关键字 / 内建 / 引擎契约符号与
// 高亮一样只从 `SlangKeywords.h` 的**唯一一张表**取 —— 旧版补全自己只有 11 条关键字,而高亮认得
// `interface`/`enum`/`where`…,于是"高亮得出、补全不出来"。本文件只留"选哪些候选 / 怎么过滤排序 /
// 悬停给什么文档",词表不再复制。
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
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace World
{
	class SlangCompletionIndex
	{
	public:
		// 清空文件符号(词表在 SlangKeywords.h,不在这里)。
		void Clear()
		{
			m_FileParams.clear();
			m_DeclaredNames.clear();
		}

		// 当前文件源码:扫描 `//! param <type> <name> = …`(解析在 SlangKeywords.h 的
		// SlangAnnotations::ParseParamDecl —— 与高亮读同一个实现),名字同时给高亮着色用。
		// 容错:少了 `//!` 后的空格(`//!param`)也认;缺名字的行跳过。重复调用替换上一次结果。
		void SetFileSource(std::string_view fileText)
		{
			SlangAnnotations::ScanParamDecls(fileText, m_FileParams);
			SlangAnnotations::CollectDeclaredNames(fileText, m_DeclaredNames);
		}

		// 本文件 `//! param` 声明的参数名(高亮把这些名字着成 Global 色;已去重)。
		const std::vector<std::string>& DeclaredNames() const { return m_DeclaredNames; }

		// 符号总数 = 词表 + 契约字段 + 文件参数(诊断/自检用,口径与 LuauCompletionIndex 同义)。
		std::size_t SymbolCount() const
		{
			return SlangSymbols::Count() + MaterialInputCount() + SurfaceCount() + m_FileParams.size();
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

		static void AppendSymbols(std::vector<LuauCompletionItem>& out, bool annotationContext)
		{
			for (std::size_t i = 0; i < SlangSymbols::Count(); ++i)
				if (SymbolInContext(SlangSymbols::kSymbols[i], annotationContext))
					out.push_back(MakeSymbolItem(SlangSymbols::kSymbols[i]));
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

		void CollectGlobals(std::vector<LuauCompletionItem>& out) const
		{
			for (const SlangAnnotations::ParamDecl& param : m_FileParams)
				out.push_back(MakeFileParamItem(param));
			// 代码上下文 = 文件参数 + 唯一词表(关键字/类型/引擎类型与函数/内建;注解词不进这里)。
			AppendSymbols(out, /*annotationContext=*/false);
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
		std::vector<std::string> m_DeclaredNames; // 给高亮用的参数名(`//! param` 声明,已去重)
	};
}
