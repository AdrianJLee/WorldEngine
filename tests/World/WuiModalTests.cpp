// D10-13:WuiModal 组件(World/WUI/Widgets/WuiModal.{h,cpp})headless 单测。
//
// 覆盖派工单要求的五条断言:
//   1. BeginModalFrame 的居中性(frame.X/Y == (视口 - size)/2,允许 0.5 像素误差);
//   2. ctx.Modal() != desc.Id → 返回 false 且不产生任何绘制命令;
//   3. ctx.Modal() == desc.Id → 返回 true,overlay 里至少两条 Rect:整视口遮罩 + 居中面板;
//   4. ModalFooter:confirmEnabled 两种取值都能跑;点击经 WuiScriptedInput 的真实输入
//      注入路径得到 ModalResult::Confirm/Cancel;无障碍节点 id/kind/enabled 正确;
//   5. BeginModalInputBlock/EndModalInputBlock 成对:遮挡期间 hover 与点击落空,End 后恢复。
//
// 纯 WuiContext 逻辑:无窗口、无渲染后端、无纹理(写法与 tests/World/WuiTests.cpp 一致)。
#include <cstdint>   // KeyCodes.h 里用 uint16_t;先包含,避免该头独立包含时缺类型
#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiScriptedInput.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiModal.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World::Wui;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// 几何比较:tolerance 默认 1e-3;居中判定按派工单放宽到 0.5 像素。
	bool Near(float a, float b, float tolerance = 0.001f)
	{
		return std::fabs(a - b) <= tolerance;
	}

	bool RectNear(const WuiRect& a, const WuiRect& b, float tolerance = 0.001f)
	{
		return Near(a.X, b.X, tolerance) && Near(a.Y, b.Y, tolerance)
			&& Near(a.W, b.W, tolerance) && Near(a.H, b.H, tolerance);
	}

	std::size_t CountKind(const std::vector<WuiDrawCommand>& commands, WuiDrawKind kind)
	{
		std::size_t count = 0;
		for (const WuiDrawCommand& command : commands)
			if (command.Kind == kind)
				++count;
		return count;
	}

	const WuiDrawCommand* FindCommand(const std::vector<WuiDrawCommand>& commands, WuiDrawKind kind,
		const WuiRect& rect, float tolerance = 0.001f)
	{
		for (const WuiDrawCommand& command : commands)
			if (command.Kind == kind && RectNear(command.Rect, rect, tolerance))
				return &command;
		return nullptr;
	}

	const WuiDrawCommand* FindText(const std::vector<WuiDrawCommand>& commands, const std::string& text)
	{
		for (const WuiDrawCommand& command : commands)
			if (command.Kind == WuiDrawKind::Text && command.Text == text)
				return &command;
		return nullptr;
	}

	// 无障碍节点拷贝:Find 返回内部 vector 的指针,下一帧 BeginFrame 就会重建,必须立刻拷出来。
	struct NodeSnapshot
	{
		bool Found = false;
		WuiId Id = 0;
		std::string Window;
		std::string Kind;
		std::string Label;
		std::string Value;
		WuiRect Rect {};
		bool Enabled = false;
		bool Interactive = false;
		bool Visible = false;
	};

	NodeSnapshot Snapshot(WuiId id)
	{
		NodeSnapshot snapshot;
		const WuiAccessNode* node = WuiAccessibility::Get().Find(id);
		if (!node)
			return snapshot;
		snapshot.Found = true;
		snapshot.Id = node->Id;
		snapshot.Window = node->Window;
		snapshot.Kind = node->Kind;
		snapshot.Label = node->Label;
		snapshot.Value = node->Value;
		snapshot.Rect = node->Rect;
		snapshot.Enabled = node->Enabled;
		snapshot.Interactive = node->Interactive;
		snapshot.Visible = node->Visible;
		return snapshot;
	}

	constexpr const char* kTestWindow = "wui-modal-test";
	constexpr const char* kModalId = "modal.test";
	constexpr const char* kOkId = "modal.test.ok";
	constexpr const char* kCancelId = "modal.test.cancel";

	ModalFrameDesc MakeDesc()
	{
		ModalFrameDesc desc;
		desc.Id = HashId(kModalId);
		desc.Title = "Modal Title";
		desc.Size = glm::vec2(420.0f, 320.0f);   // TitleHeight 用默认值 34
		return desc;
	}

	struct ModalFrameRun
	{
		bool Visible = false;
		bool EscapePressed = false;
		WuiRect Frame {};
		ModalResult Footer = ModalResult::None;
		std::size_t PlainCommands = 0;   // 普通通道:模态必须全部走 overlay,所以应为 0
		std::size_t OverlayCommands = 0;
		std::size_t OverlayRects = 0;
	};

	// 宿主真实帧序的裁剪版:BeginModalFrame → body/ModalFooter → EndModalFrame。
	// "画任何面板之前"的 BeginModalInputBlock 由第 5 组用例单独驱动。
	ModalFrameRun RunModalFrame(WuiContext& ctx, const WuiTheme& theme, const ModalFrameDesc& desc,
		const WuiInputState& input, bool confirmEnabled)
	{
		ModalFrameRun run;
		WuiAccessibility::Get().BeginFrame(kTestWindow, input.ViewportSize);
		ctx.BeginFrame(input);
		ctx.SetModal(desc.Id);
		run.Visible = BeginModalFrame(ctx, desc, &run.Frame, &run.EscapePressed, theme);
		if (run.Visible)
			run.Footer = ModalFooter(ctx, run.Frame, "OK", "Cancel",
				HashId(kOkId), HashId(kCancelId), confirmEnabled, theme);
		EndModalFrame(ctx);
		run.PlainCommands = ctx.Commands().size();
		run.OverlayCommands = ctx.OverlayCommands().size();
		run.OverlayRects = CountKind(ctx.OverlayCommands(), WuiDrawKind::Rect);
		ctx.EndFrame();
		return run;
	}

	// 脚本化输入注入:与 AI 控制通道 / 自动化同一条路径(第 1 帧 press,第 2 帧 release)。
	WuiInputState ScriptedClick(const std::string& windowKey, glm::vec2 position, glm::vec2 viewport)
	{
		WuiInputState input;
		input.ViewportSize = viewport;
		WuiScriptedInput& scripted = WuiScriptedInput::Get();
		scripted.QueueClick(windowKey, position);
		scripted.Apply(windowKey, input);
		return input;
	}

	WuiInputState ScriptedRelease(const std::string& windowKey, glm::vec2 viewport)
	{
		WuiInputState input;
		input.ViewportSize = viewport;
		WuiScriptedInput::Get().Apply(windowKey, input);
		return input;
	}

	glm::vec2 Center(const WuiRect& rect)
	{
		return glm::vec2(rect.X + rect.W * 0.5f, rect.Y + rect.H * 0.5f);
	}
}

int main()
{
	try
	{
		const WuiId modalId = HashId(kModalId);
		const WuiTheme theme;
		const ModalFrameDesc desc = MakeDesc();

		// 无障碍登记开:验证 id/kind/rect/disabled 与 AI 通道读到的完全一致。
		WuiAccessibility::Get().SetEnabled(true);

		// ---- 门控(派工 §2):ctx.Modal() != desc.Id 与 desc.Id == 0 都不画任何东西 ----
		{
			WuiContext ctx;
			WuiAccessibility::Get().BeginFrame(kTestWindow);
			WuiInputState input;
			input.ViewportSize = glm::vec2(1280.0f, 720.0f);
			input.MousePos = glm::vec2(640.0f, 360.0f);
			ctx.BeginFrame(input);
			CHECK(ctx.Modal() != modalId);
			WuiRect frame { 1.0f, 2.0f, 3.0f, 4.0f };
			bool escapePressed = false;
			CHECK(!BeginModalFrame(ctx, desc, &frame, &escapePressed, theme));
			CHECK(ctx.Commands().empty());          // 不产生任何 draw command
			CHECK(ctx.OverlayCommands().empty());
			CHECK(ctx.Commands().empty() && !escapePressed);
			CHECK(Near(frame.X, 1.0f) && Near(frame.Y, 2.0f) && Near(frame.W, 3.0f) && Near(frame.H, 4.0f));
			EndModalFrame(ctx);                      // 没 Push 就不该有 Pop 副作用
			CHECK(CountKind(ctx.Commands(), WuiDrawKind::Rect) == 0);
			ctx.EndFrame();
		}
		{
			WuiContext ctx;
			WuiInputState input;
			input.ViewportSize = glm::vec2(1280.0f, 720.0f);
			WuiAccessibility::Get().BeginFrame(kTestWindow, input.ViewportSize);
			ctx.BeginFrame(input);
			ctx.SetModal(modalId);                   // 门控通过,但 desc.Id == 0 仍必须拒绝
			ModalFrameDesc zero;
			WuiRect frame {};
			CHECK(!BeginModalFrame(ctx, zero, &frame, nullptr, theme));
			CHECK(ctx.Commands().empty() && ctx.OverlayCommands().empty());
			CHECK(Near(frame.W, 0.0f) && Near(frame.H, 0.0f));
			ctx.EndFrame();
		}

		// ---- 居中 + 遮罩 + 面板(派工 §1、§3),视口用 ctx.SetViewportSize 显式给定 ----
		{
			WuiContext ctx;
			WuiAccessibility::Get().BeginFrame(kTestWindow, glm::vec2(1000.0f, 800.0f));
			WuiInputState input;
			input.ViewportSize = glm::vec2(1280.0f, 720.0f);
			input.MousePos = glm::vec2(4.0f, 4.0f);
			ctx.BeginFrame(input);
			ctx.SetViewportSize(glm::vec2(1000.0f, 800.0f));   // BeginFrame 之后的显式覆盖
			CHECK(Near(ctx.ViewportSize().x, 1000.0f) && Near(ctx.ViewportSize().y, 800.0f));
			ctx.SetModal(modalId);

			WuiRect frame {};
			bool escapePressed = false;
			CHECK(BeginModalFrame(ctx, desc, &frame, &escapePressed, theme));
			CHECK(!escapePressed);                   // 本帧没有 Esc
			CHECK(Near(frame.X, (1000.0f - desc.Size.x) * 0.5f, 0.5f));
			CHECK(Near(frame.Y, (800.0f - desc.Size.y) * 0.5f, 0.5f));
			CHECK(Near(frame.W, desc.Size.x, 0.5f) && Near(frame.H, desc.Size.y, 0.5f));

			const std::vector<WuiDrawCommand>& commands = ctx.OverlayCommands();
			CHECK(CountKind(commands, WuiDrawKind::Rect) >= 2);   // 遮罩 + 面板(标题条也是 Rect)
			const WuiDrawCommand* mask = FindCommand(commands, WuiDrawKind::Rect, { 0.0f, 0.0f, 1000.0f, 800.0f });
			CHECK(mask != nullptr);                                // 遮罩尺寸 == 视口
			CHECK(Near(mask->Color.R, 0.0f, 0.01f) && Near(mask->Color.G, 0.0f, 0.01f));
			CHECK(Near(mask->Color.B, 0.0f, 0.01f) && Near(mask->Color.A, 0.5f, 0.01f));
			CHECK(FindCommand(commands, WuiDrawKind::Rect, frame) != nullptr);   // 面板 == 居中 frame
			CHECK(FindCommand(commands, WuiDrawKind::Rect,
				{ frame.X, frame.Y, frame.W, desc.TitleHeight }) != nullptr);    // 标题条贴顶
			CHECK(FindCommand(commands, WuiDrawKind::RectOutline, frame) != nullptr);   // 边框压在面板上
			const WuiDrawCommand* title = FindText(commands, desc.Title);
			CHECK(title != nullptr);                               // 标题文字
			CHECK(title->Rect.X >= frame.X && title->Rect.Y >= frame.Y);
			CHECK(title->Rect.Y < frame.Y + desc.TitleHeight);     // 落在标题条内
			// 通道细节不做断言:这里只关心"遮罩 + 居中面板在 overlay 通道里以正确几何存在"
			// (上面已逐条断言);绘制通道的内部路由不属于组件对外契约。

			// 标题无障碍节点:id == 模态自身,kind=text,只读。
			const NodeSnapshot titleNode = Snapshot(modalId);
			CHECK(titleNode.Found && titleNode.Id == modalId);
			CHECK(titleNode.Window == kTestWindow);
			CHECK(titleNode.Kind == "text" && titleNode.Label == desc.Title);
			CHECK(titleNode.Enabled && !titleNode.Interactive);
			CHECK(RectNear(titleNode.Rect, { frame.X, frame.Y, frame.W, desc.TitleHeight }));

			EndModalFrame(ctx);
			CHECK(CountKind(ctx.Commands(), WuiDrawKind::Rect) == 0);              // overlay 已弹出
			CHECK(ctx.OverlayCommands().size() == commands.size());                // 命令留给后端本帧绘制
			ctx.EndFrame();
		}

		// ---- 奇数尺寸:居中允许 0.5 像素误差,遮罩仍精确等于视口 ----
		{
			WuiContext ctx;
			ModalFrameDesc odd = desc;
			odd.Size = glm::vec2(301.0f, 201.0f);
			WuiInputState input;
			input.ViewportSize = glm::vec2(801.0f, 601.0f);
			WuiAccessibility::Get().BeginFrame(kTestWindow, input.ViewportSize);
			ctx.BeginFrame(input);
			ctx.SetModal(modalId);
			WuiRect frame {};
			CHECK(BeginModalFrame(ctx, odd, &frame, nullptr, theme));
			CHECK(Near(frame.X, (801.0f - 301.0f) * 0.5f, 0.5f));
			CHECK(Near(frame.Y, (601.0f - 201.0f) * 0.5f, 0.5f));
			CHECK(FindCommand(ctx.OverlayCommands(), WuiDrawKind::Rect,
				{ 0.0f, 0.0f, 801.0f, 601.0f }) != nullptr);
			EndModalFrame(ctx);
			ctx.EndFrame();
		}

		// ---- 出参可为空:调用方只关心是否可见(与 Wui::BeginModal 的 panel 出参同口径) ----
		{
			WuiContext ctx;
			WuiInputState input;
			input.ViewportSize = glm::vec2(1280.0f, 720.0f);
			WuiAccessibility::Get().BeginFrame(kTestWindow, input.ViewportSize);
			ctx.BeginFrame(input);
			ctx.SetModal(modalId);
			CHECK(BeginModalFrame(ctx, desc, nullptr, nullptr, theme));
			CHECK(!ctx.OverlayCommands().empty());
			EndModalFrame(ctx);
			ctx.EndFrame();
		}

		// ---- Esc:本帧按下时置位,其它键不置位 ----
		{
			WuiContext ctx;
			WuiInputState input;
			input.ViewportSize = glm::vec2(1280.0f, 720.0f);
			input.KeyDown = { World::KeyCodes::Escape };
			WuiAccessibility::Get().BeginFrame(kTestWindow, input.ViewportSize);
			ctx.BeginFrame(input);
			ctx.SetModal(modalId);
			bool escapePressed = false;
			CHECK(BeginModalFrame(ctx, desc, nullptr, &escapePressed, theme));
			CHECK(escapePressed);
			EndModalFrame(ctx);
			ctx.EndFrame();
		}
		{
			WuiContext ctx;
			WuiInputState input;
			input.ViewportSize = glm::vec2(1280.0f, 720.0f);
			input.KeyDown = { World::KeyCodes::Enter };
			WuiAccessibility::Get().BeginFrame(kTestWindow, input.ViewportSize);
			ctx.BeginFrame(input);
			ctx.SetModal(modalId);
			bool escapePressed = false;
			CHECK(BeginModalFrame(ctx, desc, nullptr, &escapePressed, theme));
			CHECK(!escapePressed);
			EndModalFrame(ctx);
			ctx.EndFrame();
		}

		// ---- ModalFooter(派工 §4):两个按钮都绘制/登记,点击走注入的真实输入路径 ----
		{
			WuiContext ctx;
			const glm::vec2 viewport(1000.0f, 800.0f);

			// 首帧:无点击 → None,但两个按钮都已登记(可点)。
			WuiInputState idle;
			idle.ViewportSize = viewport;
			idle.MousePos = glm::vec2(4.0f, 4.0f);
			const ModalFrameRun first = RunModalFrame(ctx, theme, desc, idle, true);
			CHECK(first.Visible && first.Footer == ModalResult::None);
			CHECK(first.PlainCommands == 0 && first.OverlayRects >= 3);

			const NodeSnapshot confirm = Snapshot(HashId(kOkId));
			const NodeSnapshot cancel = Snapshot(HashId(kCancelId));
			CHECK(confirm.Found && cancel.Found);
			CHECK(confirm.Kind == "button" && confirm.Label == "OK");
			CHECK(cancel.Kind == "button" && cancel.Label == "Cancel");
			CHECK(confirm.Window == kTestWindow && confirm.Enabled && confirm.Interactive);
			CHECK(cancel.Window == kTestWindow && cancel.Enabled && cancel.Interactive);
			CHECK(confirm.Visible && cancel.Visible);
			// 贴底内边距 + 都留在 frame 内 + 确认在左/取消在右且不重叠。
			CHECK(Near(confirm.Rect.H, ModalFooterHeight) && Near(cancel.Rect.H, ModalFooterHeight));
			CHECK(Near(confirm.Rect.Y + confirm.Rect.H, first.Frame.Y + first.Frame.H - ModalFooterPadding));
			CHECK(confirm.Rect.X >= first.Frame.X);
			CHECK(confirm.Rect.X + confirm.Rect.W <= first.Frame.X + first.Frame.W);
			CHECK(cancel.Rect.X >= confirm.Rect.X + confirm.Rect.W);
			CHECK(cancel.Rect.X + cancel.Rect.W <= first.Frame.X + first.Frame.W);
			CHECK(cancel.Rect.Y >= first.Frame.Y);
			CHECK(cancel.Rect.Y + cancel.Rect.H <= first.Frame.Y + first.Frame.H);

			// 注入点击确认(第 1 帧 press)→ Confirm;第 2 帧 release 不重复触发。
			const ModalFrameRun confirmPress = RunModalFrame(ctx, theme, desc,
				ScriptedClick(kTestWindow, Center(confirm.Rect), viewport), true);
			CHECK(confirmPress.Footer == ModalResult::Confirm);
			const ModalFrameRun confirmRelease = RunModalFrame(ctx, theme, desc,
				ScriptedRelease(kTestWindow, viewport), true);
			CHECK(confirmRelease.Footer == ModalResult::None);

			// 注入点击取消 → Cancel;release 不重复触发。
			const ModalFrameRun cancelPress = RunModalFrame(ctx, theme, desc,
				ScriptedClick(kTestWindow, Center(cancel.Rect), viewport), true);
			CHECK(cancelPress.Footer == ModalResult::Cancel);
			const ModalFrameRun cancelRelease = RunModalFrame(ctx, theme, desc,
				ScriptedRelease(kTestWindow, viewport), true);
			CHECK(cancelRelease.Footer == ModalResult::None);
			CHECK(!WuiScriptedInput::Get().HasPending());   // 注入全部消费完,不留残帧
		}

		// ---- confirmEnabled=false:弱化 + 登记 disabled,点它不产生 Confirm ----
		{
			WuiContext ctx;
			const glm::vec2 viewport(1000.0f, 800.0f);
			WuiInputState idle;
			idle.ViewportSize = viewport;
			const ModalFrameRun first = RunModalFrame(ctx, theme, desc, idle, false);
			CHECK(first.Visible && first.Footer == ModalResult::None);

			const NodeSnapshot confirm = Snapshot(HashId(kOkId));
			const NodeSnapshot cancel = Snapshot(HashId(kCancelId));
			CHECK(confirm.Found && !confirm.Enabled && !confirm.Interactive);   // ui.invoke 点不到
			CHECK(cancel.Found && cancel.Enabled && cancel.Interactive);         // 取消不受影响

			const ModalFrameRun blocked = RunModalFrame(ctx, theme, desc,
				ScriptedClick(kTestWindow, Center(confirm.Rect), viewport), false);
			CHECK(blocked.Footer == ModalResult::None);                          // 禁用确认不可点
			const ModalFrameRun blockedRelease = RunModalFrame(ctx, theme, desc,
				ScriptedRelease(kTestWindow, viewport), false);
			CHECK(blockedRelease.Footer == ModalResult::None);

			const ModalFrameRun cancelPress = RunModalFrame(ctx, theme, desc,
				ScriptedClick(kTestWindow, Center(cancel.Rect), viewport), false);
			CHECK(cancelPress.Footer == ModalResult::Cancel);
			const ModalFrameRun cancelRelease = RunModalFrame(ctx, theme, desc,
				ScriptedRelease(kTestWindow, viewport), false);
			CHECK(cancelRelease.Footer == ModalResult::None);
			CHECK(!WuiScriptedInput::Get().HasPending());
		}

		// ---- 窄框:两个按钮仍留在 frame 内且不重叠(压缩分支) ----
		{
			WuiContext ctx;
			ModalFrameDesc narrow = desc;
			narrow.Size = glm::vec2(200.0f, 160.0f);
			WuiInputState idle;
			idle.ViewportSize = glm::vec2(1000.0f, 800.0f);
			const ModalFrameRun run = RunModalFrame(ctx, theme, narrow, idle, true);
			CHECK(run.Visible && Near(run.Frame.W, 200.0f) && Near(run.Frame.H, 160.0f));
			const NodeSnapshot confirm = Snapshot(HashId(kOkId));
			const NodeSnapshot cancel = Snapshot(HashId(kCancelId));
			CHECK(confirm.Found && cancel.Found);
			CHECK(confirm.Rect.X >= run.Frame.X);
			CHECK(confirm.Rect.W >= 24.0f && cancel.Rect.W >= 24.0f);   // 压缩后不退化
			CHECK(cancel.Rect.X >= confirm.Rect.X + confirm.Rect.W);
			CHECK(cancel.Rect.X + cancel.Rect.W <= run.Frame.X + run.Frame.W);
		}

		// ---- 输入封锁(派工 §5):遮挡期间 hover 与点击都落空,End 后恢复 ----
		{
			WuiContext ctx;
			WuiInputState input;
			input.ViewportSize = glm::vec2(1280.0f, 720.0f);
			input.MousePos = glm::vec2(120.0f, 110.0f);
			input.MouseDown[0] = true;
			input.MouseClicked[0] = true;
			WuiAccessibility::Get().BeginFrame(kTestWindow, input.ViewportSize);
			ctx.BeginFrame(input);

			const WuiRect panelUnit { 100.0f, 100.0f, 120.0f, 30.0f };
			const WuiRect cornerUnit { 1200.0f, 660.0f, 70.0f, 30.0f };
			// 无遮挡:悬停命中,Button 收得到这次点击。
			CHECK(ctx.IsHovered(panelUnit));
			CHECK(Button(ctx, HashId("modal.test.behind"), panelUnit, "Behind", theme));

			BeginModalInputBlock(ctx);
			CHECK(!ctx.IsHovered(panelUnit));                  // 遮挡生效
			CHECK(!Button(ctx, HashId("modal.test.behind"), panelUnit, "Behind", theme));
			ctx.Input().MousePos = glm::vec2(1230.0f, 670.0f);  // 整视口:右下角同样落空
			CHECK(!ctx.IsHovered(cornerUnit));
			CHECK(!Button(ctx, HashId("modal.test.corner"), cornerUnit, "Corner", theme));

			EndModalInputBlock(ctx);                            // 等价于遮挡列表已清空
			ctx.Input().MousePos = glm::vec2(120.0f, 110.0f);
			CHECK(ctx.IsHovered(panelUnit));                    // hover 恢复
			CHECK(Button(ctx, HashId("modal.test.behind"), panelUnit, "Behind", theme));   // 点击恢复

			BeginModalInputBlock(ctx);                          // 二次成对调用仍生效(不是一次性)
			CHECK(!ctx.IsHovered(panelUnit));
			EndModalInputBlock(ctx);
			CHECK(ctx.IsHovered(panelUnit));

			// 再 Push 一个小遮挡区:只有它生效 → 旧的整窗遮挡确实已不在列表里。
			ctx.PushHoverBlocker({ 0.0f, 0.0f, 10.0f, 10.0f });
			ctx.Input().MousePos = glm::vec2(5.0f, 5.0f);
			CHECK(!ctx.IsHovered({ 0.0f, 0.0f, 10.0f, 10.0f }));
			ctx.Input().MousePos = glm::vec2(120.0f, 110.0f);
			CHECK(ctx.IsHovered(panelUnit));
			ctx.ClearHoverBlockers();
			ctx.EndFrame();

			// 下一帧从干净状态开始:上一帧的遮挡不会残留(模态关闭后 UI 必须恢复可交互)。
			WuiAccessibility::Get().BeginFrame(kTestWindow, input.ViewportSize);
			ctx.BeginFrame(input);
			CHECK(ctx.IsHovered(panelUnit));
			ctx.EndFrame();
		}

		WuiAccessibility::Get().SetEnabled(false);
		WuiAccessibility::Get().Clear();

		std::printf("World.WuiModal: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.WuiModal: FAILED: %s\n", error.what());
		return 1;
	}
}
