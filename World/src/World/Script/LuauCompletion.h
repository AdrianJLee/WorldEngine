#pragma once

// P2 W9.5-1:编辑器补全的符号表 + 光标上下文(纯逻辑,不依赖 WUI/VM)。
//
// 口径:
//   - 符号源一:入库存根 Game/assets/scripts/intermediate/WorldEngineAPI.luau
//     (LuaStubGenerator 渲染,World.ScriptWorkflow 的逐字节漂移门禁守着它)。行级扫描:
//     `---@class Name[: Base]`、`---@field name type desc`、`Name = {}`、
//     `function Name:Method(...)` / `function Name.Method(...)`;方法/表前连续 `---`
//     注释块的首条散文行进 Doc,块里第一个 `---@return` 的类型进 Type
//     (`---@param` / `---@overload` / `---@operator` 与未知 tag 忽略)。
//   - 符号源二:SetFileSource 传入的当前脚本文件(`---@class X[: Base]`/`local x`/
//     `function x`/`X = {...}`),作为"文件内符号"参与无接收者查询,并给 `self.` 提供类成员。
//   - 符号源三:Luau 关键字(Keyword)与沙箱允许的内置库/基础函数(Global;清单与
//     LuauVm.cpp 的 kAllowedLibraries + lbaselib 减去 kForbiddenGlobals 对齐)。
//
// Query 流程 = 上下文(光标前片段)→ 候选(接收者成员 / 全局+文件符号+关键字)→
// 过滤(前缀大小写不敏感;前缀零命中时子串兜底)→ 排序(Field→Method→Global→Class→
// Keyword,同档按名字大小写不敏感升序)→ maxItems 截断。
// 索引**不**判断"光标是否在字符串/注释里":那是编辑器拿 LuauHighlighter 的 token 做的事。

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace World
{
	// 一条补全候选。Type/Doc 允许为空(例如关键字与本地符号)。
	struct LuauCompletionItem
	{
		std::string Name;
		std::string Type;
		std::string Doc;

		enum class Kind : uint8_t
		{
			Field,     // ---@field 注解字段(含 WorldScript 的 OnCreate? 这类可选字段)
			Method,    // function X:Y / function X.Y
			Global,    // 全局表(X = {})、文件内符号、Luau 沙箱内置
			Keyword,   // Luau 关键字/字面量
			Class,     // 只有 ---@class 注解、没有运行时全局的名字(WorldScript/组件类)
		};
		// 冻结接口的字段名与枚举名相同,同名成员会遮蔽枚举类型(C2597),
		// 外部引用枚举请用这个别名:`item.Kind == LuauCompletionItem::KindType::Method`。
		using KindType = Kind;
		// 字段名与类型同名是冻结接口的一部分;`= {}` 值初始化到第一个枚举值 Field(0),
		// 避免在初始化式里再写 Kind::(那时 Kind 已被成员名遮蔽)。
		Kind Kind = {};
	};

	class WLD_API LuauCompletionIndex
	{
	public:
		// 清空全部符号(存根 + 文件符号 + 关键字/内置)。
		void Clear();

		// 解析存根文本。空文本返回 false 并填 error(可传 null);其余畸形输入按"跳过该行"
		// 容错处理,不抛异常、不截断。成功后可继续 SetFileSource。
		bool LoadStub(std::string_view text, std::string* error);

		// 读取并解析存根文件(通常是 WLD_ASSETPATH/scripts/intermediate/WorldEngineAPI.luau);
		// 读不到文件返回 false 并填 error(可传 null)。
		bool LoadStubFile(const std::string& path, std::string* error);

		// 当前脚本文件源码:行级扫描 ---@class X[: Base]、local x、function x、X = {...}
		// 作为文件内符号(默认 Kind=Global;只有 ---@class 没有赋值时是 Class),并记录文件类
		// 供 self. 解析。重复调用替换上一次的文件符号。
		void SetFileSource(std::string_view fileText);

		// 符号总数 = 顶层查询项(全局/类/关键字/内置/文件符号)+ 各类成员(字段/方法)。
		std::size_t SymbolCount() const;

		// 按光标前片段补全:先清空 out,再填候选,最多 maxItems 条(0 = 不截断)。
		void Query(std::string_view linePrefix, std::size_t maxItems,
			std::vector<LuauCompletionItem>& out) const;

		// 悬停提示:按上下文(linePrefix = 词之前的整行片段)解析 word 的类型与文档。
		// 找到返回 true 并填 out;未找到返回 false。
		bool Describe(std::string_view linePrefix, std::string_view word, LuauCompletionItem& out) const;

		// 光标前片段 → 接收者 / 分隔符('.',':') / 前缀标识符。无接收者时 separator='\0'。
		// 例:"ui.bu" → ("ui",'.',"bu");"entity:Get" → ("entity",':',"Get");
		// "local x = en" → ("",'\0',"en");"for i = 1, " → ("",'\0',"")。
		static void ParseContext(std::string_view linePrefix, std::string& receiver,
			char& separator, std::string& prefix);

	private:
		struct ClassInfo
		{
			std::string Name;
			std::string Base;
			std::vector<LuauCompletionItem> Members;
		};

		void ParseStubText(std::string_view text);
		void ParseFileText(std::string_view text);
		const ClassInfo* ResolveClass(std::string_view name) const;
		void CollectClassMembers(const ClassInfo* info,
			std::vector<const LuauCompletionItem*>& out) const;
		void CollectGlobals(std::vector<const LuauCompletionItem*>& out, bool includeKeywords) const;

		std::vector<LuauCompletionItem> m_Items;   // 存根顶层项 + 关键字 + 沙箱内置
		std::vector<LuauCompletionItem> m_Tags;    // `---@` 注解标签(只在注解上下文给候选)
		std::vector<ClassInfo> m_StubClasses;
		std::vector<LuauCompletionItem> m_FileItems;   // 文件内符号(SetFileSource)
		std::vector<ClassInfo> m_FileClasses;          // 文件内 ---@class(通常 1 个)
		std::unordered_map<std::string, std::size_t> m_ItemByName;   // 名字 → m_Items 下标(去重)
	};
}
