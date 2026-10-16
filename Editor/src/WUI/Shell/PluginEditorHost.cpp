#include "wldpch.h"
#include "WUI/Shell/PluginEditorHost.h"

#include "WUI/Shell/EditorShell.h"
#include "World/Plugins/PluginManager.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	namespace
	{
		// 单次面板绘制的宿主状态:通过 WeEditorUiApi.UserData 传给每个 C 回调,
		// 栈上存活、只在本帧 draw 调用期间有效。
		// 支持垂直滚动(s_PanelScrollY / s_PanelContentH)与裁剪保护,容纳长列表/任务/对话面板。
		struct PluginPanelDrawState
		{
			PluginEditorHost* Host = nullptr;
			Plugins::PluginManager* Manager = nullptr;
			Wui::WuiContext* Ctx = nullptr;
			Wui::WuiRect Rect { 0, 0, 0, 0 };
			Wui::WuiRect Body { 0, 0, 0, 0 };
			Wui::WuiTheme Theme;
			Plugins::PluginEditorPanel Panel;
			float StartY = 0.0f;
			float ScrollY = 0.0f;
			float Cursor = 0.0f;
			bool Broken = false;
			// 无 id 的 Label 自动编号(`plugin.panel.<插件>.<面板>.#<n>`),让每个文本行都有
			// 稳定可寻址的 a11y 节点(探针/读屏都能看到插件面板里的文本)。
			uint32_t TextIndex = 0;
		};

		static std::unordered_map<std::string, float> s_PanelScrollY;
		static std::unordered_map<std::string, float> s_PanelContentH;

		inline constexpr float kPluginPanelPad = 10.0f;
		inline constexpr float kPluginPanelRowGap = 4.0f;
		inline constexpr float kPluginPanelControlHeight = 24.0f;
		inline constexpr float kPluginPanelLabelHeight = 18.0f;
		inline constexpr float kPluginPanelSeparatorHeight = 13.0f;
		inline constexpr float kPluginScrollbarWidth = 6.0f;
		inline constexpr float kPluginMaxContentSpan = 24000.0f;

		PluginPanelDrawState* DrawStateOf(void* uiContext)
		{
			auto* state = static_cast<PluginPanelDrawState*>(uiContext);
			return (state && state->Host && state->Ctx) ? state : nullptr;
		}

		// 面板内的控件 a11y id = 面板命名空间 + 插件自己的短 id(宿主负责隔离)。
		Wui::WuiId ControlId(const PluginPanelDrawState& state, const char* id, const char* fallback)
		{
			const char* suffix = (id && id[0]) ? id : fallback;
			return Wui::HashId((state.Panel.DisplayId + "." + (suffix ? suffix : "")).c_str());
		}

		// 单值控件的宽度:按真实文本度量(带上下限),不依赖固定像素猜测。
		float LabelWidth(PluginPanelDrawState& state, const char* text)
		{
			const std::string label = text ? text : "";
			const float measured = state.Ctx->MeasureTextWidth(label, state.Theme.FontSizeBody,
				Wui::WuiFontFamily::Ui);
			const float maxAvail = std::max(88.0f, state.Rect.W - kPluginPanelPad * 2.0f - kPluginScrollbarWidth);
			return std::min(std::max(measured + 18.0f, 88.0f), maxAvail);
		}

		void Advance(PluginPanelDrawState& state, float height)
		{
			state.Cursor += height + kPluginPanelRowGap;
		}

		bool OutOfRoom(const PluginPanelDrawState& state)
		{
			return (state.Cursor - state.StartY) > kPluginMaxContentSpan;
		}

		void RegisterPanelRootNode(PluginPanelDrawState& state)
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(state.Panel.DisplayId.c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = state.Panel.DisplayId;
			node.Kind = "panel";
			node.Label = state.Panel.Title;
			node.Value = state.Panel.PluginId;
			node.Rect = state.Rect;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// ---- 小组件表实现(宿主侧;每个函数都从 uiContext 找回本次 Draw 的状态)----

		void UiLabel(void* uiContext, const char* text)
		{
			auto* state = DrawStateOf(uiContext);
			if (!state || OutOfRoom(*state) || !text)
				return;

			std::string_view sv(text);
			size_t start = 0;
			while (start < sv.size())
			{
				if (OutOfRoom(*state)) break;
				size_t newline = sv.find((char)10, start);
				std::string line = (newline == std::string_view::npos)
					? std::string(sv.substr(start))
					: std::string(sv.substr(start, newline - start));

				if (!line.empty())
				{
					Wui::Label(*state->Ctx, { state->Rect.X + kPluginPanelPad, state->Cursor }, line,
						state->Theme.Text, state->Theme.FontSizeBody);
				}
				Advance(*state, kPluginPanelLabelHeight);

				if (newline == std::string_view::npos) break;
				start = newline + 1;
			}
		}

		bool UiButton(void* uiContext, const char* id, const char* label)
		{
			auto* state = DrawStateOf(uiContext);
			if (!state || OutOfRoom(*state))
				return false;
			const Wui::WuiRect rect { state->Rect.X + kPluginPanelPad, state->Cursor,
				LabelWidth(*state, label), kPluginPanelControlHeight };
			const bool clicked = Wui::Button(*state->Ctx, ControlId(*state, id, "button"), rect,
				label ? label : "", state->Theme);
			Advance(*state, kPluginPanelControlHeight);
			return clicked;
		}

		void UiSeparator(void* uiContext)
		{
			auto* state = DrawStateOf(uiContext);
			if (!state || OutOfRoom(*state))
				return;
			const float y = state->Cursor + kPluginPanelSeparatorHeight * 0.5f;
			Wui::LineSegment(*state->Ctx,
				{ state->Rect.X + kPluginPanelPad, y },
				{ state->Rect.X + std::max(10.0f, state->Rect.W - kPluginPanelPad - kPluginScrollbarWidth), y },
				state->Theme.Border, 1.0f);
			Advance(*state, kPluginPanelSeparatorHeight);
		}

		bool UiCheckbox(void* uiContext, const char* id, const char* label, bool* value)
		{
			auto* state = DrawStateOf(uiContext);
			if (!state || !value || OutOfRoom(*state))
				return false;
			const Wui::WuiRect rect { state->Rect.X + kPluginPanelPad, state->Cursor,
				LabelWidth(*state, label), kPluginPanelControlHeight };
			bool mutableValue = *value;
			const bool changed = Wui::Checkbox(*state->Ctx, ControlId(*state, id, "checkbox"), rect,
				label ? label : "", mutableValue, state->Theme);
			if (changed)
				*value = mutableValue;
			Advance(*state, kPluginPanelControlHeight);
			return changed;
		}

		bool UiInvokeCommand(void* uiContext, const char* commandId)
		{
			auto* state = DrawStateOf(uiContext);
			if (!state || !commandId || !commandId[0])
				return false;
			std::string error;
			if (state->Manager->InvokeEditorCommand(commandId, &error))
				return true;
			// 失败不静默:面板里给一行可见提示(插件自己也可以据此回显)。
			if (!OutOfRoom(*state))
			{
				Wui::Label(*state->Ctx, { state->Rect.X + kPluginPanelPad, state->Cursor },
					"[plugin] command failed: " + error, state->Theme.Warning, state->Theme.FontSizeSmall);
				Advance(*state, kPluginPanelLabelHeight);
			}
			return false;
		}

		void UiLabelColored(void* uiContext, const char* text, int tone)
		{
			auto* state = DrawStateOf(uiContext);
			if (!state || !text || OutOfRoom(*state))
				return;
			Wui::WuiColor col = state->Theme.Text;
			if (tone == 1) col = state->Theme.Success;
			else if (tone == 2) col = state->Theme.Warning;
			else if (tone == 3) col = state->Theme.Danger;
			else if (tone == 4) col = state->Theme.Accent;
			else if (tone == 5) col = state->Theme.TextMuted;

			std::string_view sv(text);
			size_t start = 0;
			while (start < sv.size())
			{
				if (OutOfRoom(*state)) break;
				size_t newline = sv.find((char)10, start);
				std::string line = (newline == std::string_view::npos)
					? std::string(sv.substr(start))
					: std::string(sv.substr(start, newline - start));

				if (!line.empty())
				{
					Wui::Label(*state->Ctx, { state->Rect.X + kPluginPanelPad, state->Cursor }, line,
						col, state->Theme.FontSizeBody);
				}
				Advance(*state, kPluginPanelLabelHeight);

				if (newline == std::string_view::npos) break;
				start = newline + 1;
			}
		}

		bool UiTextInput(void* uiContext, const char* id, char* buffer, uint32_t capacity)
		{
			auto* state = DrawStateOf(uiContext);
			if (!state || !buffer || capacity == 0 || OutOfRoom(*state))
				return false;
			const Wui::WuiRect rect { state->Rect.X + kPluginPanelPad, state->Cursor,
				std::max(100.0f, state->Rect.W - kPluginPanelPad * 2.0f - kPluginScrollbarWidth), kPluginPanelControlHeight };
			std::string stdBuf(buffer);
			const bool submitted = Wui::TextField(*state->Ctx, ControlId(*state, id, "text_input"), rect,
				stdBuf, state->Theme);
			if (!stdBuf.empty())
			{
				std::strncpy(buffer, stdBuf.c_str(), capacity - 1);
				buffer[capacity - 1] = 0;
			}
			else
			{
				buffer[0] = 0;
			}
			Advance(*state, kPluginPanelControlHeight);
			return submitted;
		}

		bool UiTabBar(void* uiContext, const char* id, const char* const* tabs, uint32_t count, int* active)
		{
			auto* state = DrawStateOf(uiContext);
			if (!state || !tabs || count == 0 || !active || OutOfRoom(*state))
				return false;
			std::vector<std::string> tabList;
			tabList.reserve(count);
			for (uint32_t i = 0; i < count; ++i)
				tabList.push_back(tabs[i] ? tabs[i] : "");
			const Wui::WuiRect rect { state->Rect.X + kPluginPanelPad, state->Cursor,
				std::max(100.0f, state->Rect.W - kPluginPanelPad * 2.0f - kPluginScrollbarWidth), kPluginPanelControlHeight };
			int currentActive = *active;
			const bool changed = Wui::TabBar(*state->Ctx, ControlId(*state, id, "tab_bar"), rect,
				tabList, currentActive, state->Theme);
			if (changed)
				*active = currentActive;
			Advance(*state, kPluginPanelControlHeight + 4.0f);
			return changed;
		}
	}

	PluginEditorHost::PluginEditorHost(EditorShell& shell) : m_Shell(shell) {}

	bool PluginEditorHost::RegisterEditorCommand(const std::string& pluginId,
		const Plugins::WeEditorCommandDesc& desc, Plugins::WeEditorCommandCallback callback, void* userData)
	{
		// E2 口径:启动器形态没有编辑器命令面(项目相关能力整体缺席)。
		if (m_Shell.IsLauncherMode())
			return false;
		const std::string rawId = desc.Id ? desc.Id : "";
		if (rawId.empty() || !callback)
			return false;
		const std::string name = "plugin.command." + pluginId + "." + rawId;
		if (m_Commands.count(name) > 0)
			return false;
		Command command;
		command.PluginId = pluginId;
		command.RawId = rawId;
		command.Callback = callback;
		command.UserData = userData;
		m_Commands.emplace(name, std::move(command));
		return true;
	}

	bool PluginEditorHost::UnregisterEditorCommand(const std::string& pluginId, const std::string& id)
	{
		// 幂等:没注册过也返回 true(卸载兜底路径会重复调用)。
		m_Commands.erase("plugin.command." + pluginId + "." + id);
		return true;
	}

	bool PluginEditorHost::RegisterEditorPanel(const std::string& pluginId,
		const Plugins::WeEditorPanelDesc& desc, Plugins::PluginEditorPanelRenderFn render, void* userData)
	{
		if (m_Shell.IsLauncherMode())
			return false;
		const std::string rawId = desc.Id ? desc.Id : "";
		if (rawId.empty() || !render)
			return false;
		const std::string displayId = "plugin.panel." + pluginId + "." + rawId;
		if (m_Panels.count(displayId) > 0)
			return false;
		Panel panel;
		panel.PluginId = pluginId;
		panel.RawId = rawId;
		panel.DisplayId = displayId;
		panel.Render = render;
		panel.UserData = userData;
		m_Panels.emplace(displayId, std::move(panel));
		// 面板实例/布局在**插件加载全部结束之后**再补建(见 EditorShell::EnsurePluginPanelsFromRegistry),
		// 这样注册中途失败不会留下半个布局。
		return true;
	}

	bool PluginEditorHost::UnregisterEditorPanel(const std::string& pluginId, const std::string& id)
	{
		const std::string displayId = "plugin.panel." + pluginId + "." + id;
		m_Panels.erase(displayId);
		// 已经打开的窗口由 EditorShell 在帧末兜底关闭(它每帧核对注册表;这里不直接动 UI)。
		return true;
	}

	int PluginEditorHost::RenderEditorPanelSurface(const Plugins::PluginEditorPanel& panel,
		Plugins::WeEditorPanelDrawFn draw, void* userData)
	{
		if (!draw || !m_Shell.HasWuiContext())
			return -1;

		PluginPanelDrawState state;
		state.Host = this;
		state.Manager = m_Shell.GetPluginManager();
		state.Ctx = &m_Shell.WuiContextRef();
		state.Rect = m_Shell.CurrentPluginPanelRect();
		state.Theme = Wui::CurrentTheme();
		state.Panel = panel;

		const float topInset = kPluginPanelPad + kPluginPanelLabelHeight;
		state.Body = Wui::WuiRect {
			state.Rect.X,
			state.Rect.Y + topInset,
			state.Rect.W,
			std::max(1.0f, state.Rect.H - topInset)
		};

		float& scrollY = s_PanelScrollY[panel.DisplayId];
		const float prevContentH = s_PanelContentH[panel.DisplayId];
		const float prevMaxScroll = std::max(0.0f, prevContentH - state.Body.H);

		if (state.Body.Contains(state.Ctx->Input().MousePos) && state.Ctx->Input().Wheel != 0.0f)
		{
			scrollY -= state.Ctx->Input().Wheel * 36.0f;
		}
		scrollY = std::max(0.0f, std::min(scrollY, prevMaxScroll));

		state.ScrollY = scrollY;
		state.StartY = state.Body.Y - scrollY;
		state.Cursor = state.StartY;

		Plugins::WeEditorUiApi ui;
		ui.UserData = &state;
		ui.PluginId = panel.PluginId.c_str();
		ui.Label = &UiLabel;
		ui.Button = &UiButton;
		ui.Separator = &UiSeparator;
		ui.Checkbox = &UiCheckbox;
		ui.InvokeCommand = &UiInvokeCommand;
		ui.LabelColored = &UiLabelColored;
		ui.TextInput = &UiTextInput;
		ui.TabBar = &UiTabBar;

		RegisterPanelRootNode(state);
		state.Ctx->PushClipRect(state.Body);
		const int result = (draw)(userData, &ui, &state);
		state.Ctx->PopClipRect();

		const float totalContentH = std::max(0.0f, (state.Cursor - state.StartY) + kPluginPanelPad);
		s_PanelContentH[panel.DisplayId] = totalContentH;
		const float maxScroll = std::max(0.0f, totalContentH - state.Body.H);
		scrollY = std::max(0.0f, std::min(scrollY, maxScroll));

		if (totalContentH > state.Body.H + 1.0f)
		{
			const Wui::WuiRect scrollBarRect {
				state.Body.X + state.Body.W - kPluginScrollbarWidth - 2.0f,
				state.Body.Y + 2.0f,
				kPluginScrollbarWidth,
				std::max(12.0f, state.Body.H - 4.0f)
			};
			Wui::ScrollBar(*state.Ctx,
				Wui::HashId((panel.DisplayId + ".scrollbar").c_str()),
				scrollBarRect,
				totalContentH,
				state.Body.H,
				scrollY,
				state.Theme,
				false);
		}

		if (result != 0)
			WLD_CORE_ERROR("[plugin] editor panel '{0}' draw returned {1} (contract violation)",
				panel.DisplayId, result);
		return result;
	}

	std::vector<std::string> PluginEditorHost::PanelIds() const
	{
		std::vector<std::string> ids;
		ids.reserve(m_Panels.size());
		for (const auto& entry : m_Panels)
			ids.push_back(entry.first);
		std::sort(ids.begin(), ids.end());
		return ids;
	}

	std::string PluginEditorHost::PanelTitle(const std::string& panelId) const
	{
		Plugins::PluginManager* manager = m_Shell.GetPluginManager();
		Plugins::PluginEditorPanel panel;
		if (!manager || !manager->FindEditorPanel(panelId, &panel))
			return std::string();
		return panel.Title;
	}

	bool PluginEditorHost::InvokeEditorCommand(const std::string& commandName, std::string* error)
	{
		if (!m_Commands.count(commandName))
		{
			if (error) *error = "no plugin editor command named '" + commandName + "'";
			return false;
		}
		Plugins::PluginManager* manager = m_Shell.GetPluginManager();
		if (!manager)
		{
			if (error) *error = "plugin manager is not available";
			return false;
		}
		return manager->InvokeEditorCommand(commandName, error);
	}
}

