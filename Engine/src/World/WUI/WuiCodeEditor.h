#pragma once

#include "World/Core/Export.h"
#include "World/Script/LuauCompletion.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiTextBuffer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace World::Wui
{
	enum class WuiCodeTokenKind : uint8_t
	{
		Default,
		Keyword,
		String,
		Comment,
		Number,
		Global,
		Operator,
		// WUI-MAT-INTEL4:给高亮加"类别色" —— 类型 / 函数 / 字段(成员) / 注解关键字各一色,
		// 让"类型标识"与"名字/变量"在视觉上分开(值与顺序**追加**在末尾,旧 kind 的索引不变)。
		Type,
		Function,
		Field,
		Annotation,
	};

	// 一行的语法 token:偏移是相对该行首的字节偏移(行文本不含 '\n' 与行尾 '\r')。
	struct WuiCodeToken
	{
		uint32_t StartByte = 0;
		uint32_t EndByte = 0;
		WuiCodeTokenKind Kind = WuiCodeTokenKind::Default;
	};

	// ---- MAT-UI6a:查找/替换与同词高亮共用的匹配器 ----
	// 会话缩放的夹取范围与步进(Ctrl+滚轮按步进、Ctrl+0 回 1.0)。宿主要显示/持久化时用同一组常量。
	constexpr float kCodeEditorZoomMin = 0.5f;
	constexpr float kCodeEditorZoomMax = 3.0f;
	constexpr float kCodeEditorZoomStep = 0.05f;

	// 匹配开关(与查找条上的两个开关一一对应;正则本批不做,留位不实现)。
	struct WuiCodeFindOptions
	{
		// "Aa":true = 区分大小写。关掉时做 **ASCII 大小写折叠**(A-Z ↔ a-z),非 ASCII 字节按原样比较。
		bool CaseSensitive = false;
		// "ab":true = 全词 —— 命中两侧都不能是标识符码点(口径与编辑器的词边界一致:
		// [0-9A-Za-z_] 与 >=0x80 的码点都算词内)。
		bool WholeWord = false;
	};

	// 一次命中:UTF-8 字节区间 [Start,End),落在码点边界上。
	struct WuiCodeFindMatch
	{
		size_t Start = 0;
		size_t End = 0;
	};

	// 在 text 里找出 needle 的全部出现(不重叠、按 Start 升序);needle 为空 → 空表。
	// 查找条计数/上下跳、同词高亮、单测都走这一份实现(不复制匹配逻辑)。
	WLD_API std::vector<WuiCodeFindMatch> FindCodeMatches(std::string_view text, std::string_view needle,
		const WuiCodeFindOptions& options);
	// 从 fromOffset 起找下一个命中(Start >= fromOffset);没有则**回绕**到第一个。
	// 空表 → -1(不区分大小写/全词的开关:matches 已经是按开关算好的结果)。
	WLD_API int NextCodeMatchIndex(const std::vector<WuiCodeFindMatch>& matches, size_t fromOffset);
	// 上一个命中(Start < fromOffset);没有则回绕到最后一个。空表 → -1。
	WLD_API int PrevCodeMatchIndex(const std::vector<WuiCodeFindMatch>& matches, size_t fromOffset);

	struct WuiCodeEditorOptions
	{
		float FontSize = 14.0f;
		float LineHeight = 20.0f;
		float GutterWidth = 0.0f; // 0 = 自动(按最大行号位数)
		bool ReadOnly = false;
		// MAT-UI6a:会话缩放因子。生效字号 = FontSize × UiZoom,生效行高 = LineHeight × UiZoom;
		// 内核把它夹到 [kCodeEditorZoomMin, kCodeEditorZoomMax]。Ctrl+滚轮改的就是它、Ctrl+0 复位 1.0,
		// 结果通过 WuiCodeEditorResult 回报 —— **内核不写任何偏好文件**,持久化归宿主。
		// 只在编辑器实例第一次出现时作为初值;之后以内核状态为准(宿主读 result.UiZoom 回写自己的会话值)。
		float UiZoom = 1.0f;
		// 逐行语法高亮回调:line = 行显示文本(不含换行),out = 该行 token(可留空隙,
		// 空隙按 Default 着色绘制)。必须按 StartByte 升序返回。
		std::function<void(std::string_view line, std::vector<WuiCodeToken>& out)> Highlight;
		// 剪贴板注入(W9-2 接系统剪贴板;测试注入内存串)。返回 false 表示无剪贴板内容。
		std::function<bool(std::string& out)> GetClipboard;
		std::function<bool(std::string_view text)> SetClipboard;
		// W9.5 补全 provider:linePrefix = 光标所在行、行首到光标处的 UTF-8 片段;
		// 返回空 = 不弹浮层。空 = 本控件不做补全(默认)。
		std::function<void(std::string_view linePrefix,
			std::vector<World::LuauCompletionItem>& out)> Completion;
		// 补全浮层的无障碍节点 id 前缀:<前缀>.<i>(候选)、<前缀>.status(状态)。
		std::string CompletionIdPrefix = "editor.suggest";
		// W9.6 悬停提示 provider:linePrefix = 鼠标所在词之前的整行片段(含接收者),
		// word = 鼠标下的标识符。返回 true 时用 out 的 Name/Type/Doc 画提示框;空 = 不提示。
		std::function<bool(std::string_view linePrefix, std::string_view word,
			World::LuauCompletionItem& out)> Hover;
		// W9.7 语法错误行(0-based;-1 = 无):画红色底 + 下划线。
		int ErrorLine = -1;
		// MAT-UI45(纯追加,向后兼容):逐可见行装饰回调 —— 在**本行文本画完之后、本帧浮层
		// (补全/Hover)之前**调用,调用方可在行尾补画标记(如材质注解的 Color 色块)。
		//   line      = 0 基行号;
		//   lineRect  = 该行文本带(已含滚动偏移;X/W = 文本区,高 = LineHeight,不含行号栏);
		//   textEndX  = 本行文本右端 x(行内 pen 终点,已含文本区偏移)。
		// 空 = 不装饰(默认,旧调用点行为零变化)。裁剪作用域仍是文本区(与正文同进同出)。
		std::function<void(WuiContext&, int line, const WuiRect& lineRect, float textEndX)> LineDecorator;
	};

	struct WuiCodeEditorResult
	{
		bool Changed = false;
		// Ctrl+S:只置位,由宿主决定保存什么(编辑器不碰文件系统)。
		bool SaveRequested = false;
		// MAT-UI6a:本帧 Ctrl+滚轮 / Ctrl+0 改了会话缩放 → 宿主据此刷新指示/写自己的会话值。
		bool ZoomChanged = false;
		// 当前**生效**的会话缩放(每帧都回报;宿主不必自己夹取,也不用猜内核口径)。
		float UiZoom = 1.0f;
	};

	// 多行代码编辑控件:可见行裁剪绘制 + 行号栏 + 当前行高亮 + 选区 + caret 闪烁;
	// 滚轮/拖动滚动 + 竖向滚动条;真实字形度量的鼠标命中;上下/Home/End/PageUp/PageDown/
	// Ctrl+←→/Shift 选区/Ctrl+A/C/X/V/Z/Y/Tab/Enter/Backspace/Delete;双击选词。
	// ReadOnly = true 时只导航/选择/复制,不改 buffer。
	WLD_API WuiCodeEditorResult CodeEditor(WuiContext& ctx, WuiId id, const WuiRect& rect,
		WuiTextBuffer& buffer, const WuiCodeEditorOptions& options);
}
