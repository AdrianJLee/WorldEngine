#pragma once

// P2 W9-2:Luau 逐行语法高亮(token 级,不做语义着色)。
//
// 口径:
//   - 纯函数式逐行接口 HighlightLine:输入行首状态、输出行尾状态。跨行的**块注释**
//     (--[[ .. ]] / --[==[ .. ]==])与**长括号字符串**([[ .. ]] / [=[ .. ]=])靠这个
//     状态在行间延续 —— 编辑器只画可见行,不能靠"从头 token 化到这一行"。
//   - 输出 Wui::WuiCodeToken(StartByte/EndByte 相对该行起点,升序、互不重叠);
//     空隙表示 Default(WuiCodeEditor 会按 Default 补齐)。
//   - 逐字节扫描但不在多字节 UTF-8 序列内断开:引号/转义/注释标记都是 ASCII,
//     中文字符只可能落在字符串/注释 token 内部(中文串边界用例)。

#include "World/Core/Export.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiTextBuffer.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace World
{
	// 行间延续状态:-1 = 不在块注释/长字符串中;>=0 = 该结构的 '=' 个数(0 = [[ / --[[)。
	struct LuauHighlightState
	{
		int BlockCommentLevel = -1;
		int LongStringLevel = -1;

		bool operator==(const LuauHighlightState& other) const
		{
			return BlockCommentLevel == other.BlockCommentLevel
				&& LongStringLevel == other.LongStringLevel;
		}
		bool operator!=(const LuauHighlightState& other) const { return !(*this == other); }
	};

	class WLD_API LuauHighlighter
	{
	public:
		// 高亮一行:state 为行首状态,返回后 state 为该行行尾状态(交给下一行)。
		// line 不含换行符(允许带行尾 '\r',会被当作空白)。
		// engineTypes(VEC-A7,可空):引擎外部类名集合(vec2/vec3/vec4/mat3/mat4/Entity/WorldScript)。
		// 传了 → 命中的标识符发 EngineType(覆盖 Default/Global,但不覆盖关键字/常量/self/成员名);
		// 不传 → 输出与加这个参数之前逐字节一致(老调用方零影响)。
		// fileSymbols(VEC-H1,可空):当前文件的"数据字段 + 局部"名字集合(见 LuauFileSymbolSet)。
		// 传了 → 本文件的字段在字段位置发 Field、本文件的局部名保持 Default(都不再落回 Global/EngineType);
		// 不传/空 → 与加这个参数之前逐字节一致(存根服务表 `Level` 依旧 Global)。
		static void HighlightLine(std::string_view line, LuauHighlightState& state,
			std::vector<Wui::WuiCodeToken>& out, const class LuauEngineTypeSet* engineTypes = nullptr,
			const class LuauFileSymbolSet* fileSymbols = nullptr);
	};

	// 引擎外部类名集合(有序去重;唯一职责 = "这个名字是不是引擎类")。
	// 名单由调用方从**既有来源**(完成索引的引擎类型档 / 存根 ---@class)填进来,本类不内置第二份名单。
	class WLD_API LuauEngineTypeSet
	{
	public:
		void Set(std::vector<std::string> names);
		void Clear();
		bool Empty() const { return m_Names.empty(); }
		bool Contains(std::string_view name) const;
		// 名字集合的内容指纹(面板据此判断"要不要让高亮缓存失效")。
		uint64_t Hash() const { return m_Hash; }
		// 只读名字表(字典序;诊断/探针用,不参与判定)。
		const std::vector<std::string>& Names() const { return m_Names; }

	private:
		std::vector<std::string> m_Names;   // 字典序去重
		uint64_t m_Hash = 0;
	};

	// VEC-H1:当前文件的"文件内符号"集合(数据字段 + 局部),唯一职责 =
	// "这个名字在本文件里是数据字段/局部,而不是存根声明的全局服务表"。
	// 名单由调用方从**既有来源**(完成索引的文件符号 / 表构造字段推断 / `---@field` 注解)填进来,
	// 本类不内置第二份名单。
	//
	// 分两档的原因(用户口径:同一屏里"数据字段 Level"与"服务表 Level"必须能区分开):
	//   - Fields = 本文件里作为**字段名**出现过的名字(表构造键 `{ Level = 3 }`、成员 `t.Level`、
	//     字段赋值 `t.Level = 3`、`---@field Level` 注解)→ 在**字段位置**染 Field(成员/字段色);
	//   - Locals = 本文件声明过的局部/文件符号(`local x` / `function x` / `X = {}`)→ 裸用法
	//     保持 Default(本文件的局部名不再落回 Global/EngineType)。
	// 只有**没在本文件里作为字段/局部出现过**的名字才保留原语义 —— 存根服务表 `Level = {}`
	// 的裸用法(`Level.Primary()`)依旧 Global 浅蓝。
	class WLD_API LuauFileSymbolSet
	{
	public:
		void Set(std::vector<std::string> locals, std::vector<std::string> fields);
		void Clear();
		bool Empty() const { return m_Locals.empty() && m_Fields.empty(); }
		bool ContainsLocal(std::string_view name) const;
		bool ContainsField(std::string_view name) const;
		// 名字集合的内容指纹(面板/缓存据此判断"要不要让高亮缓存失效";两档分开参与哈希)。
		uint64_t Hash() const { return m_Hash; }
		// 只读名字表(字典序;诊断/探针用,不参与判定)。
		const std::vector<std::string>& Locals() const { return m_Locals; }
		const std::vector<std::string>& Fields() const { return m_Fields; }

	private:
		std::vector<std::string> m_Locals;   // 字典序去重
		std::vector<std::string> m_Fields;   // 字典序去重
		uint64_t m_Hash = 0;
	};

	// 按行 token 缓存(面板每帧调用 Update;WuiCodeEditor 的 Highlight 回调按行取用):
	//   - 只有"内容或行首延续状态变了"的行才重新 token 化,其余行直接复用上一帧结果;
	//   - Find 用行文本指针 + 长度 + 内容指纹定位(编辑导致缓冲区重分配时自然失配,
	//     调用方 Update 后重试即可)。
	class WLD_API LuauHighlightCache
	{
	public:
		// 更新到 buffer 的当前内容(buffer.Revision() 与行数都没变时零成本返回)。
		// engineTypes(VEC-A7,可空):集合内容指纹变化时缓存全量失效(调用方不需要自己记得 Clear)。
		void Update(const Wui::WuiTextBuffer& buffer, const LuauEngineTypeSet* engineTypes = nullptr);
		// VEC-H1:把"当前文件的字段/局部名"喂给高亮(文件名集合变化 → 缓存全量失效)。
		// 不调用 = 集合为空 = 旧行为逐字节不变。Update / HighlightLine 都会带上它。
		void SetFileSymbols(std::vector<std::string> locals, std::vector<std::string> fields);
		const LuauFileSymbolSet& FileSymbols() const { return m_FileSymbols; }
		void Clear();

		// 该行的 token(行号越界返回空表)。
		const std::vector<Wui::WuiCodeToken>& Tokens(int line) const;
		// 按行文本查找(未命中返回 nullptr:调用方 Update 后重试或现场按行首状态兜底)。
		const std::vector<Wui::WuiCodeToken>* Find(std::string_view line) const;
		int LineCount() const { return m_LineCount; }

	private:
		struct Entry
		{
			uint64_t Hash = 0;
			LuauHighlightState Start;
			LuauHighlightState End;
			const char* Text = nullptr;
			size_t TextSize = 0;
			std::vector<Wui::WuiCodeToken> Tokens;
		};

		std::vector<Entry> m_Lines;
		std::unordered_map<const char*, size_t> m_ByPointer;
		uint64_t m_Revision = ~0ull;
		uint64_t m_EngineTypeHash = 0;
		LuauFileSymbolSet m_FileSymbols;
		uint64_t m_FileSymbolHash = 0;
		int m_LineCount = -1;
	};
}
