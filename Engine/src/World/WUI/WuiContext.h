#pragma once

#include "World/WUI/WuiCore.h"
#include "World/WUI/WuiOperationLog.h"
#include "World/WUI/WuiUndoStack.h"
#include "World/Core/Export.h"
#include "World/Core/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace World::Wui
{
	struct WuiInputState
	{
		glm::vec2 MousePos { 0, 0 };
		bool MouseDown[3] = { false, false, false };
		bool MouseClicked[3] = { false, false, false };
		bool MouseReleased[3] = { false, false, false };
		bool MouseDoubleClicked[3] = { false, false, false };
		float Wheel = 0;
		bool WantKeyboard = false;
		bool Ctrl = false, Shift = false, Alt = false;
		std::vector<uint32_t> KeyDown;   // 引擎 KeyCodes
		// 本帧新按下(沿)与 OS 重复事件:文本编辑器这类"按键=一次动作"的控件必须用它们,
		// 否则一次按下会跨多帧重复触发(实测:一次回车插入多行)。
		std::vector<uint32_t> KeyPressed;
		std::vector<uint32_t> KeyRepeated;
		std::vector<uint32_t> TextInput; // 本帧输入字符(UTF-32)
		glm::vec2 ViewportSize { 1280, 720 };
		float FPS = 0;
	};

	// 可聚焦控件的登记项(P4-UX6 焦点顺序表):控件在绘制时登记,登记顺序 = 视觉顺序。
	// Rect 保留给宿主做"把获得焦点的控件滚进视野"之类后续用途,当前只参与登记。
	struct WuiFocusable
	{
		WuiId Id = 0;
		WuiRect Rect { 0, 0, 0, 0 };
	};

	// P4-U28:一次"点击"的按下归属。press 帧由第一个命中的控件登记(IsClicked /
	// IsClickCompleted),release 帧只有落在同一个控件上的那次释放才算"完成了一次点击"。
	// 这样弹层关闭那一帧的 release 不会落到正下方的控件上,也不会把拖拽松开算成点击。
	struct WuiClickOwner
	{
		WuiId Id = 0;
		WuiRect Rect { 0, 0, 0, 0 };
		bool Valid = false;
	};

	// P4-U28:覆盖层矩形(弹出菜单/下拉/模态外框)。
	//  - Depth = 登记时的 overlay 绘制深度:只挡**比它浅**的控件(常驻面板 depth 0,
	//    模态内容 depth 1,弹层叠在模态里 depth 2 …),所以"弹层盖住模态里的其它行"
	//    也成立,而弹层自己/更深的子菜单不受影响(P4-U7 的口径是"只挡 depth 0",
	//    在模态内部会漏挡 —— M3 报告的 release 穿透真因)。
	//  - Delayed = "弹层已关闭、只多挡一帧"的残留矩形(关闭帧 + 一帧,两帧后彻底清除)。
	struct WuiOverlayRect
	{
		WuiRect Rect { 0, 0, 0, 0 };
		int Depth = 0;
		bool Delayed = false;
	};

	enum class WuiDrawKind : uint8_t
	{
		Rect,
		RectOutline,
		Text,
		Image,
		// 任意四边形(顶点按左上/右上/右下/左下顺序):画斜线、箭头、圆环等
		// 轴对齐矩形覆盖不到的形状。4 个顶点放在 WuiDrawCommand::Vertices。
		Quad,
		// P4-U10:四角渐变矩形(顶点顺序同 Quad,颜色取 WuiDrawCommand::Corners)。
		// 取色器的 饱和度×明度方块 / 色相条 / alpha 条 都靠它:2D 管线本来就是逐顶点颜色,
		// 所以"渐变"只是 4 个顶点给不同颜色,后端与着色器都不用改。
		Gradient,
		ClipPush,
		ClipPop,
	};

	struct WuiDrawCommand
	{
		WuiDrawKind Kind = WuiDrawKind::Rect;
		WuiRect Rect;
		WuiColor Color;
		float Rounding = 0;
		float Thickness = 1;
		std::string Text;
		float FontSize = 15;
		bool Bold = false;
		uint64_t Image = 0;
		WuiRect Uv { 0, 0, 1, 1 };
		int TextSelStart = -1;
		int TextSelEnd = -1;
		int TextCursorByte = -1;
		// Quad 命令的 4 个顶点(屏幕坐标);其余命令忽略。
		std::array<glm::vec2, 4> Vertices { glm::vec2 { 0, 0 }, glm::vec2 { 0, 0 }, glm::vec2 { 0, 0 }, glm::vec2 { 0, 0 } };
		// W9:字体族选择(默认 Ui,追加在末尾保持既有聚合初始化兼容)。
		WuiFontFamily Family = WuiFontFamily::Ui;
		// W9:代码编辑器的闪烁 caret(逐行局部字节偏移,-1 = 不画)。与 TextCursorByte
		// 的区别:该字段由后端做 0.5s 闪烁,且按真实度量画 1.5px。
		int TextCaretByte = -1;
		// Gradient 命令的四个角颜色(左上/右上/右下/左下)。
		std::array<WuiColor, 4> Corners { WuiColor { 1, 1, 1, 1 }, WuiColor { 1, 1, 1, 1 },
			WuiColor { 1, 1, 1, 1 }, WuiColor { 1, 1, 1, 1 } };
	};

	// ---- 文本度量钩子 ----
	// WUI 核心是纯逻辑,不知道字体;渲染后端在初始化时注册真实字形度量,headless
	// 测试可注入假度量(逐码点宽度自定)。未注册时回退为估算宽度(ASCII 0.6em,
	// 其余 1.0em),保证纯逻辑路径不依赖后端。
	using WuiTextMeasureFn = std::function<float(std::string_view utf8, float fontSize, WuiFontFamily family)>;
	// owner 用于注销:只有注册者本人可以清除钩子(多后端/多窗口下不会互相踩)。
	void WLD_API SetTextMeasureHook(void* owner, WuiTextMeasureFn fn);
	void WLD_API ClearTextMeasureHook(void* owner);
	float WLD_API MeasureTextWithHook(std::string_view utf8, float fontSize, WuiFontFamily family);

	// P4-UX2c:UI 内容缩放(用户反馈"全屏后 UI 还是有点小")。
	// 语义是"整块 UI 的缩放":布局坐标 = 物理像素 / UiScale,渲染时由视口映射放大,
	// 文字按 UiScale 栅格化后仍以设计单位摆放。输入坐标必须除以它,否则命中会偏。
	WLD_API float UiScale();
	WLD_API void SetUiScale(float scale);

	// ---- 全局文本焦点登记 ----
	// 仿 WuiAccessibility:多个窗口(WuiContext 实例)共享一份登记,每帧由各窗口重建。
	// 用途:GLFW 事件在 UI 帧之后分发,EditorLayer::OnKeyPressed 只能用"上一帧登记的
	// 文本焦点"决定按键是给文本控件还是引擎全局快捷键(W9-2 的三层路由)。
	class WLD_API WuiTextFocus
	{
	public:
		struct Entry
		{
			WuiId Id = 0;
			std::string Window; // "main" / "float:<面板>"(WuiContext::SetWindowKey 显式登记,不依赖控制通道)
			std::string Panel;  // 面板 id
			// P1c-E4-fix:单行文本框的 Tab 交还标记。true = 下一次 Tab/Shift+Tab 不被文本控件吞掉,
			// 走正常焦点链(单行框里 Tab 没有文本语义);多行编辑器(CodeEditor)保持 false ⇒ Tab=缩进不变。
			bool ReleaseOnTab = false;
		};

		static WuiTextFocus& Get();

		// 每个窗口(上下文)在 BeginFrame 时调用:清掉本上下文上一帧的登记。
		void BeginContextFrame(const void* context);
		// 取得焦点的文本控件在绘制时调用(每帧重建)。
		void Set(const void* context, WuiId id, std::string window, std::string panel, bool releaseOnTab = false);
		void Clear();

		// 当前是否存在持有焦点的文本控件。
		bool Active() const { return !m_Entries.empty(); }
		// 最近登记的文本焦点(多窗口同时有文本焦点时,以最后一次绘制登记为准);无则 nullptr。
		const Entry* Current() const { return m_Entries.empty() ? nullptr : &m_Entries.back().Info; }
		WuiId Id() const { return m_Entries.empty() ? 0 : m_Entries.back().Info.Id; }
		const std::string& Window() const;
		const std::string& Panel() const;
		// 最近登记的文本焦点是否带"Tab 交还"标记(无登记 = false)。
		bool ReleaseOnTab() const { return !m_Entries.empty() && m_Entries.back().Info.ReleaseOnTab; }

	private:
		struct Item
		{
			const void* Context = nullptr;
			Entry Info;
		};
		std::vector<Item> m_Entries;
	};

	// 帧级上下文:输入、持久状态、样式栈、焦点、绘制命令。
	class WuiContext
	{
	public:
		WuiContext();
		// 销毁时清掉本窗口的文本焦点登记:否则关掉的窗口会永远吞掉全局快捷键。
		~WuiContext();

		void BeginFrame(const WuiInputState& input);
		void EndFrame();

		WuiInputState& Input() { return m_Input; }
		const WuiInputState& Input() const { return m_Input; }
		std::vector<WuiDrawCommand>& Commands() { return m_OverlayDepth > 0 ? m_OverlayCommands : m_Commands; }
		const std::vector<WuiDrawCommand>& OverlayCommands() const { return m_OverlayCommands; }
		// 进入/退出顶层绘制:弹出菜单、模态、tooltip、焦点环等画在普通 UI 之上。
		// 只影响**绘制顺序**;命中由 RegisterOverlayRect(下一帧的矩形遮挡)+
		// ConsumePointerClick(打开那一下的点击归属)负责 —— 不能让"画在上层"顺带
		// 关掉后面所有控件的命中,否则面板中途画的 tooltip 也会把后面的控件打死。
		void PushOverlay() { ++m_OverlayDepth; }
		void PopOverlay()
		{
			if (m_OverlayDepth > 0)
				--m_OverlayDepth;
		}
		// P4-U7:登记一个**覆盖层矩形**(弹出菜单/下拉/模态外框)。它在下一帧变成"下层遮挡区":
		// 只挡非覆盖层控件(m_OverlayDepth == 0),覆盖层自己不受影响 —— 这样"先画的面板"
		// 也不会吃掉落在弹出层上的点击(菜单栏菜单、面板弹出菜单、下拉弹层都走这一条)。
		// 立即模式里"后画的盖住先画的"只对绘制成立,命中必须靠这一条补齐。
		// 覆盖层每帧打开时都要调(矩形可以逐帧变化)。登记时记录当时的 overlay 深度:
		// 它决定这条遮挡区挡到哪一层(见 WuiOverlayRect);关闭后它会按深度多挡一帧。
		void RegisterOverlayRect(const WuiRect& rect)
		{
			m_OverlayRects.push_back(WuiOverlayRect { rect, m_OverlayDepth, false });
		}
		// P4-U7:消费本帧的这次点击。凡是"打开弹出层/模态"的动作都该调用它 ——
		// 否则打开用的那一下点击会继续被后面绘制的控件看到(实测:打开菜单那一下
		// 同时按到了菜单项/底部按钮)。
		void ConsumePointerClick(int button = 0)
		{
			if (button >= 0 && button < 3)
				m_PointerConsumedClick[button] = true;
		}
		bool IsPointerClickConsumed(int button = 0) const
		{
			return button >= 0 && button < 3 && m_PointerConsumedClick[button];
		}
		// 不带任何"上层遮挡/捕获"判定的原始命中(弹出层的"点外关闭"用它:弹层自己算内外)。
		bool HitTestRaw(const WuiRect& rect, glm::vec2 point) const { return World::Wui::HitTest(rect, point); }
		WuiStyleSheet& Sheet() { return m_Sheet; }

		// P4-UX4:悬停提示(tooltip)。控件在悬停时登记文本(后登记覆盖先登记),
		// 宿主画完面板后调用 `Wui::DrawTooltip` 把它画到 overlay 层 —— 这样提示不被
		// 面板裁剪、也不会压住后续控件。BeginFrame 清空,所以只有"本帧仍悬停"的项会显示。
		void SetTooltip(std::string text) { m_Tooltip = std::move(text); }
		const std::string& Tooltip() const { return m_Tooltip; }

		template <typename T>
		T& Persist(WuiId id, const T& initial)
		{
			struct Holder : WuiStateBase
			{
				Holder() { TypeName = typeid(T).name(); }
				T Value;
			};
			auto it = m_State.find(id);
			if (it == m_State.end())
			{
				auto holder = std::make_shared<Holder>();
				holder->Value = initial;
				m_State[id] = holder;
				return static_cast<Holder*>(holder.get())->Value;
			}
			if (!it->second->TypeName || std::strcmp(it->second->TypeName, typeid(T).name()) != 0)
				WLD_CORE_ERROR("WUI persisted state id {0} reused with different types ({1} vs {2})",
					id, it->second->TypeName ? it->second->TypeName : "null", typeid(T).name());
			return static_cast<Holder*>(it->second.get())->Value;
		}

		void PushStyle(const WuiStyle& style);
		void PopStyle();
		WuiStyle CurrentStyle() const;

		void SetFocus(WuiId id) { m_Focus = id; }
		WuiId Focus() const { return m_Focus; }
		// ---- 焦点顺序与键盘导航(P4-UX6 / U2A)----
		// 可聚焦控件在**绘制时**调用:注册顺序即视觉顺序,Tab / Shift+Tab 按它移动焦点、末尾回卷
		// (回卷与"当前焦点不在表里"的取法复用 WuiCore 的 NextFocus)。
		// BeginFrame 把上一帧的表挪走并清空本帧表 → 每帧重建;EndFrame 时"上一帧登记过、本帧没再登记"
		// 的焦点 id 自动清除(面板关闭/控件消失不留幽灵焦点)。代码编辑器、视口这类由面板自管焦点的
		// id 从不在表里,不受这条规则影响 —— 它们的 Tab/Escape 语义由自己处理。
		void RegisterFocusable(WuiId id, const WuiRect& rect);
		// ---- 滚动裁剪栈(U2A 补)----
		// BeginScrollArea/EndScrollArea 压栈/出栈;焦点环画在 overlay 层(不受普通裁剪影响),
		// 需要用它判断"这个矩形还看得见吗",否则滚出视口的控件会留下漂在外面的焦点环。
		void PushClipRect(const WuiRect& rect);
		void PopClipRect();
		bool ClipAllows(const WuiRect& rect) const;
		// W9 review:窗口/面板身份由宿主显式告知,不依赖仅在 --ai-control 下才启用的
		// WuiAccessibility(否则日常会话里文本焦点登记的 Window/Panel 为空,快捷键路由错位)。
		void SetWindowKey(std::string windowKey) { m_WindowKey = std::move(windowKey); }
		void SetPanelId(std::string panelId) { m_PanelId = std::move(panelId); }
		const std::string& WindowKey() const { return m_WindowKey; }
		const std::string& PanelId() const { return m_PanelId; }
		// 文本输入态 + 全局文本焦点登记(聚焦的文本控件每帧调用一次 SetTextInputActive(true))。
		// releaseOnTab(P1c-E4-fix):单行文本框传 true —— Tab/Shift+Tab 交还给焦点顺序表;
		// 默认 false = 沿用"文本持焦时 Tab 归文本控件"(多行 CodeEditor 的缩进语义)。
		void SetTextInputActive(bool active, bool releaseOnTab = false);
		bool IsTextInputActive() const { return m_TextInputActive; }
		// 文本度量(命中测试/行宽/横向滚动)。family 决定字体族,后端注册真实度量。
		float MeasureTextWidth(std::string_view utf8, float fontSize, WuiFontFamily family = WuiFontFamily::Ui) const;
		void SetCursor(WuiCursor cursor) { m_Cursor = cursor; }
		WuiCursor Cursor() const { return m_Cursor; }
		uint64_t Frame() const { return m_Frame; }
		WuiOperationLog& Ops() { return m_Ops; }
		const WuiOperationLog& Ops() const { return m_Ops; }
		WuiUndoStack& History() { return m_History; }
		void RecordOp(std::string category, std::string action, std::string target, std::string detail);
		bool IsKeyPressed(uint32_t keyCode) const
		{
			return std::find(m_Input.KeyDown.begin(), m_Input.KeyDown.end(), keyCode) != m_Input.KeyDown.end();
		}
		// 本帧新按下(不含长按重复) / 本帧 OS 重复事件 / 两者取并(一次动作)。
		bool WasKeyPressed(uint32_t keyCode) const
		{
			return std::find(m_Input.KeyPressed.begin(), m_Input.KeyPressed.end(), keyCode)
				!= m_Input.KeyPressed.end();
		}
		bool WasKeyRepeated(uint32_t keyCode) const
		{
			return std::find(m_Input.KeyRepeated.begin(), m_Input.KeyRepeated.end(), keyCode)
				!= m_Input.KeyRepeated.end();
		}
		bool WasKeyTriggered(uint32_t keyCode) const { return WasKeyPressed(keyCode) || WasKeyRepeated(keyCode); }
		bool IsHovered(const WuiRect& rect) const { return HitTest(rect, m_Input.MousePos); }
		bool IsClicked(const WuiRect& rect, int button = 0) const
		{
			// P4-U7:本帧这一次点击已被上层消费(打开弹出层/模态)→ 后面的控件不再看到它。
			if (IsPointerClickConsumed(button))
				return false;
			if (!HitTest(rect, m_Input.MousePos) || !m_Input.MouseClicked[button])
				return false;
			// P4-U28:按下这一帧记录这次按下的归属(release 帧由 IsClickCompleted 核对)。
			RecordClickOwner(button, 0, rect);
			return true;
		}
		// P4-U28:release 帧的点击确认 —— press 与 release 必须落在**同一个控件**上。
		// 弹层条目(下拉选项、可搜索下拉候选、取色器预设色块)走这条,于是:
		//  - 在条目上按下 → 拖到条目外松开:不选中(动作只在 release 且同控件时发生);
		//  - 弹层关闭那一帧落到下层的 release:下层控件的归属对不上,不会被当成自己的点击。
		// 同帧内按下+抬起(帧间隔吞掉了一次快速点击)仍按"第一个命中者"确认,保留快速点击手感。
		bool IsClickCompleted(const WuiRect& rect, int button = 0) const
		{
			return IsClickCompleted(0, rect, button);
		}
		bool IsClickCompleted(WuiId id, const WuiRect& rect, int button = 0) const
		{
			if (IsPointerClickConsumed(button))
				return false;
			if (m_Input.MouseClicked[button])
			{
				if (!HitTest(rect, m_Input.MousePos))
					return false;
				const bool owned = m_ClickOwners[button].Valid;
				RecordClickOwner(button, id, rect);
				// 同一帧里 press+release(快速点击):按下归属即本次点击,仍只认第一个命中者。
				return m_Input.MouseReleased[button] && !owned;
			}
			if (!m_Input.MouseReleased[button])
				return false;
			const WuiClickOwner& owner = m_ClickOwners[button];
			if (!owner.Valid)
				return false;
			// 有稳定 id 的控件按 id 核对(矩形可能因悬停/布局微调而变);没 id 的老控件按矩形。
			const bool sameRect = owner.Rect.X == rect.X && owner.Rect.Y == rect.Y
				&& owner.Rect.W == rect.W && owner.Rect.H == rect.H;
			const bool same = (id != 0 && owner.Id != 0) ? (owner.Id == id) : sameRect;
			return same && HitTest(rect, m_Input.MousePos);
		}
		bool IsDoubleClicked(const WuiRect& rect, int button = 0) const
		{
			// 与 IsClicked 同一口径:被打开覆盖层那一下消费掉的点击不算双击。
			if (IsPointerClickConsumed(button))
				return false;
			return HitTest(rect, m_Input.MousePos) && m_Input.MouseDoubleClicked[button];
		}

		// P4-U30:菜单项的"release 确认"。统一口径 = press 落在(该菜单的按钮 ∪ 该菜单的面板)
		// 范围内,release 落在某一菜单项上 → 触发;其余一律不触发:
		//  - 菜单栏经典路径:press 在菜单按钮上(那一下打开了弹层)→ 滑到项 → 松开 = 触发;
		//  - 面板路径:press 落在弹层面板内(项上或面板空白),release 落在同一块面板的项上 = 触发;
		//  - 在项上按下 → 拖走 → 松开 = 不触发;在面板外(如视口)按下 → 拖到项上松开 = 不触发。
		// RecordMenuPress 在 press 帧登记归属(不再按下即动作),IsMenuRelease 在 release 帧核对。
		void RecordMenuPress(WuiId id, const WuiRect& rect, int button = 0) const;
		bool IsMenuRelease(WuiId id, const WuiRect& rect, int button = 0) const;

		// ---- 弹窗/模态 ----
		void OpenPopup(WuiId id);
		void ClosePopup(WuiId id);
		void CloseAllPopups() { m_OpenPopups.clear(); m_PopupOpenFrame.clear(); }
		bool IsPopupOpen(WuiId id) const;
		bool ClosePopupsOnOutsideClick(const std::vector<WuiId>& popups, const WuiRect& ignoreRect);
		// P4-U7:模态**打开**的那一帧同样消费点击 —— 模态是在本帧稍后画的,
		// 不消费的话"打开它的那一下"会落到模态里恰好同位置的控件上。
		void SetModal(WuiId id)
		{
			if (id != 0 && id != m_Modal)
				ConsumePointerClick(0);
			m_Modal = id;
		}
		void ClearModal() { m_Modal = 0; }
		WuiId Modal() const { return m_Modal; }
		void SetViewportSize(glm::vec2 size) { m_ViewportSize = size; }
		glm::vec2 ViewportSize() const { return m_ViewportSize; }

		// ---- 拖拽/放置 ----
		void BeginDrag(WuiId id, const std::string& payload);
		bool IsDragActive(std::string* payload) const;
		void EndDrag();
		// 拖动期间每帧调用以保持目标矩形;释放后 AcceptDrop 返回 true。
		bool DropTarget(const WuiRect& rect);
		// 仅当 payload 以指定前缀开头时武装落点(区分面板拖拽与文件拖拽)。
		bool DropTarget(const WuiRect& rect, const std::string& payloadPrefix);
		// 每帧构建 UI 前调用:清空上一帧的落点,只保留本帧悬停命中的目标。
		void ClearDropTarget() { m_DropArmed = false; }
		bool AcceptDrop(std::string* payload);
		// 仅当 payload 以指定前缀开头时才消费;不匹配返回 false 且不消耗,
		// 把 drop 留给声明了匹配前缀的其他消费者。
		bool AcceptDrop(std::string* payload, const std::string& payloadPrefix);

		// ---- 悬停遮挡区(叠放在上层的浮动面板用)----
		// 本 UI 的命中测试就是矩形包含判定,没有 z 序;窗口内浮动面板画在停靠区之上时,
		// 需要把它的矩形登记成遮挡区,让下面的面板在命中测试里判为未命中,
		// 否则浮动面板与它下面的面板会同时响应同一次点击。
		// 用法:画下层之前 Push,画浮动面板之前 Clear(浮动面板自身要能命中)。
		void PushHoverBlocker(const WuiRect& rect) { m_HoverBlockers.push_back(rect); }
		void ClearHoverBlockers() { m_HoverBlockers.clear(); }

	private:
		// 带遮挡区判定的命中测试:IsHovered/IsClicked/DropTarget 都走它。
		bool HitTest(const WuiRect& rect, glm::vec2 point) const;
		// press 帧登记"这次按下归哪个控件"。只认第一个命中者(一次按下只有一个归属)。
		void RecordClickOwner(int button, WuiId id, const WuiRect& rect) const;
		// P4-U30:press 与 item 是否落在**同一块**已登记的弹层面板里(本帧登记 + 上一帧登记)。
		bool PressInPanelWith(const WuiRect& item, int button) const;
		// Tab / Shift+Tab / Escape 的焦点导航(在 BeginFrame 里、清空本帧登记之后调用)。
		// textFocusActive = 上一帧结束时文本控件仍持有焦点 → 这些键全归文本控件,焦点表不抢。
		// textReleaseOnTab = 上一帧的文本焦点带"Tab 交还"标记(单行文本框)⇒ Tab/Shift+Tab 除外。
		void NavigateFocus(const WuiInputState& input, bool textFocusActive, bool textReleaseOnTab);
		struct WuiStateBase
		{
			const char* TypeName = nullptr;
			virtual ~WuiStateBase() = default;
		};

		WuiInputState m_Input;
		std::vector<WuiDrawCommand> m_Commands;
		std::vector<WuiDrawCommand> m_OverlayCommands;
		int m_OverlayDepth = 0;
		// P4-U7:本帧登记的覆盖层矩形 → 下一帧作为"下层遮挡区"(只挡更浅的控件)。
		// P4-U28:再保留上一帧的集合,用来把"上一帧刚关闭"的弹层矩形多挡一帧(两帧后彻底清)。
		std::vector<WuiOverlayRect> m_OverlayRects;
		std::vector<WuiOverlayRect> m_OverlayRectsLast;
		std::vector<WuiOverlayRect> m_UnderlayBlockers;
		// P4-U7:本帧已消费的鼠标键(打开弹出层那一下不再穿透)。
		bool m_PointerConsumedClick[3] = { false, false, false };
		// P4-U28:一次按下的归属(press 帧写,release 帧核对;IsClicked/IsClickCompleted 是 const)。
		mutable WuiClickOwner m_ClickOwners[3];
		// P4-U30:菜单项的按下区间(press 帧写位置,按住期间一直有效,release 帧核对后失效)。
		glm::vec2 m_PressPos[3] { glm::vec2 { 0, 0 }, glm::vec2 { 0, 0 }, glm::vec2 { 0, 0 } };
		bool m_PressValid[3] = { false, false, false };
		// 这一次按住是否打开了某个弹层(菜单栏按钮路径:press 在按钮上,release 落在菜单项上)。
		bool m_PressOpenedPopup[3] = { false, false, false };
		std::unordered_map<WuiId, std::shared_ptr<WuiStateBase>> m_State;
		std::vector<WuiStyle> m_StyleStack;
		WuiStyleSheet m_Sheet;
		WuiId m_Focus = 0;
		std::vector<WuiFocusable> m_Focusables;     // 本帧(绘制中累加)
		std::vector<WuiFocusable> m_FocusablesPrev; // 上一帧(Tab 顺序 + "消失即失焦"判定)
		std::vector<WuiRect> m_ClipStack;           // 滚动区裁剪栈(空 = 不裁剪)
		bool m_TextInputActive = false;
		// P1c-E4-fix:上一帧文本焦点是否要求"Tab 交还"(与 m_TextInputActive 同一生命周期)。
		bool m_TextInputReleaseOnTab = false;
		std::string m_WindowKey;
		std::string m_PanelId;
		std::string m_Tooltip;
		WuiCursor m_Cursor = WuiCursor::Arrow;
		std::vector<WuiId> m_OpenPopups;
		std::unordered_map<WuiId, uint64_t> m_PopupOpenFrame;
		WuiId m_Modal = 0;
		glm::vec2 m_ViewportSize { 1280, 720 };
		bool m_Dragging = false;
		WuiId m_DragId = 0;
		std::string m_DragPayload;
		bool m_DropArmed = false;
		bool m_DropAccepted = false;
		bool m_DragPending = false;
		std::vector<WuiRect> m_HoverBlockers;
		glm::vec2 m_DragPressPos { 0, 0 };
		uint64_t m_Frame = 0;
		WuiOperationLog m_Ops;
		WuiUndoStack m_History;
	};
}
