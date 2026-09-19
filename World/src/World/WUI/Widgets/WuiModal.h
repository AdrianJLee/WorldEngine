#pragma once

// 模态窗口组件:把编辑器里"居中 + 遮罩 + 真正挡输入 + Esc + 标题/按钮条"这套模态行为
// 收口到一处,不再让每个弹窗各写一套。既有的 Wui::BeginModal/EndModal 保持不变
// (其它模态仍在用);新代码按三段式使用:
//   1) 宿主画**任何面板之前** BeginModalInputBlock,画模态本体之前 EndModalInputBlock;
//   2) BeginModalFrame/EndModalFrame 画外框(遮罩 + 面板 + 标题栏);
//   3) ModalFooter 画底部按钮条并返回点击结果(确认/取消)。
// 组件不持有"打开/关闭"状态:调用方决定何时 ctx.SetModal/ClearModal —— 外框只在
// ctx.Modal() == desc.Id 的那一帧绘制,与 BeginModal 的门控语义一致。

#include "World/WUI/WuiWidgets.h"

#include <string>

namespace World::Wui
{
	// 一帧的模态输入封锁:宿主在**画任何面板之前**调用,画模态本体之前 EndModalInputBlock 解开。
	// 语义 = 原来 shell 里那套 PushHoverBlocker(整窗) → 面板照常绘制但命中全部落空 → 画模态前 Clear。
	// 必须成对:Begin 之后同一帧内必须 End(否则模态自己的控件也点不动)。
	void BeginModalInputBlock(WuiContext& ctx);
	void EndModalInputBlock(WuiContext& ctx);

	// 居中的模态外框:遮罩 + 面板 + 标题栏。内部按 ctx.ViewportSize() 居中(沿用 BeginModal 的算法)。
	// 仅在 ctx.Modal() == desc.Id 时绘制;返回 true = 本帧模态可见,调用方继续画 body,并在结尾
	// 调用 EndModalFrame。Esc 在本帧按下时把 escapePressed 置 true(调用方决定是否关闭)。
	struct ModalFrameDesc
	{
		WuiId Id = 0;
		std::string Title;
		glm::vec2 Size { 420.0f, 320.0f };
		float TitleHeight = 34.0f;
	};
	bool BeginModalFrame(WuiContext& ctx, const ModalFrameDesc& desc, WuiRect* frameRect,
		bool* escapePressed, const WuiTheme& theme);
	void EndModalFrame(WuiContext& ctx);

	// 底部按钮条:自动放在 frameRect 底部,返回点击结果(取消/确认)。
	// 两个按钮都绘制、都登记无障碍节点(button;confirmEnabled=false 时登记为 disabled 且不可点)。
	enum class ModalResult
	{
		None = 0,
		Confirm,
		Cancel,
	};
	// ModalFooter 占用的内边距与按钮高度:宿主用它给状态行等 body 预留底部空间。
	inline constexpr float ModalFooterPadding = 16.0f;
	inline constexpr float ModalFooterHeight = 30.0f;
	ModalResult ModalFooter(WuiContext& ctx, const WuiRect& frameRect, const std::string& confirmLabel,
		const std::string& cancelLabel, WuiId confirmId, WuiId cancelId, bool confirmEnabled,
		const WuiTheme& theme);
}
