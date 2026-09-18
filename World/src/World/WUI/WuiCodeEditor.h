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
	};

	// 一行的语法 token:偏移是相对该行首的字节偏移(行文本不含 '\n' 与行尾 '\r')。
	struct WuiCodeToken
	{
		uint32_t StartByte = 0;
		uint32_t EndByte = 0;
		WuiCodeTokenKind Kind = WuiCodeTokenKind::Default;
	};

	struct WuiCodeEditorOptions
	{
		float FontSize = 14.0f;
		float LineHeight = 20.0f;
		float GutterWidth = 0.0f; // 0 = 自动(按最大行号位数)
		bool ReadOnly = false;
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
	};

	struct WuiCodeEditorResult
	{
		bool Changed = false;
		// Ctrl+S:只置位,由宿主决定保存什么(编辑器不碰文件系统)。
		bool SaveRequested = false;
	};

	// 多行代码编辑控件:可见行裁剪绘制 + 行号栏 + 当前行高亮 + 选区 + caret 闪烁;
	// 滚轮/拖动滚动 + 竖向滚动条;真实字形度量的鼠标命中;上下/Home/End/PageUp/PageDown/
	// Ctrl+←→/Shift 选区/Ctrl+A/C/X/V/Z/Y/Tab/Enter/Backspace/Delete;双击选词。
	// ReadOnly = true 时只导航/选择/复制,不改 buffer。
	WLD_API WuiCodeEditorResult CodeEditor(WuiContext& ctx, WuiId id, const WuiRect& rect,
		WuiTextBuffer& buffer, const WuiCodeEditorOptions& options);
}
