#include "wldpch.h"
#include "PluginEditorHost.h"

#include "EditorShell.h"

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace World
{
	namespace
	{
		// ---- 面板 Draw 上下文(只读句柄;插件只原样回传给 WeEditorUiApi 的函数)----
		//
		// 小组件表按 Draw 调用顺序**垂直排列**:每个控件占一行(Label/Separator 更矮),
		// 由本结构里的游标推进;面板高度不够时停止绘制(插件面板首期不做滚动)。
	struct PluginPanelDrawState
	{
			PluginEditorHost* Host = nullptr;
			Plugins::PluginManager* Manager = nullptr;
			Wui::WuiContext* Ctx = nullptr;
			Wui::WuiRect Rect { 0, 0, 0, 0 };
			Wui::WuiTheme Theme;
			Plugins::PluginEditorPanel Panel;
			float Cursor = 0.0f;
			bool Broken = false;
			// 无 id 的 Label 自动编号(`plugin.panel.<插件>.<面板>.#<n>`),让每个文本行都有
			// 稳定可寻址的 a11y 节点(探针/读屏都能看到插件面板里的文本)。
			uint32_t TextIndex = 0;
		};

		inline constexpr float kPluginPanelPad = 10.0f;
		inline constexpr float kPluginPanelRowGap = 4.0f;
		inline constexpr float kPluginPanelControlHeight = 24.0f;
		inline constexpr float kPluginPanelLabelHeight = 18.0f;
		inline constexpr float kPluginPanelSeparatorHeight = 13.0f;

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
			return std::min(std::max(measured + 18.0f, 88.0f), std::max(88.0f, state.Rect.W - 20.0f));
		}

		void Advance(PluginPanelDrawState& state, float height)
		{
			state.Cursor += height + kPluginPanelRowGap;
		}

		bool OutOfRoom(const PluginPanelDrawState& state)
		{
			return state.Cursor > state.Rect.Y + state.Rect.H - kPluginPanelPad;
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
			Wui::Label(*state->Ctx, { state->Rect.X + kPluginPanelPad, state->Cursor }, text,
				state->Theme.Text, state->Theme.FontSizeBody);
			const std::string autoId = "#" + std::to_string(state->TextIndex++);
			Wui::WuiAccessNode node;
			node.Id = ControlId(*state, autoId.c_str(), "label");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = state->Panel.DisplayId;
			node.Kind = "label";
			node.Label = text;
			node.Rect = { state->Rect.X + kPluginPanelPad, state->Cursor,
				std::max(10.0f, state->Rect.W - kPluginPanelPad * 2.0f), kPluginPanelLabelHeight };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			Advance(*state, kPluginPanelLabelHeight);
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
				{ state->Rect.X + std::max(10.0f, state->Rect.W - kPluginPanelPad), y },
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
		state.Cursor = state.Rect.Y + kPluginPanelPad + kPluginPanelLabelHeight;

		Plugins::WeEditorUiApi ui;
		ui.UserData = &state;
		ui.PluginId = panel.PluginId.c_str();
		ui.Label = &UiLabel;
		ui.Button = &UiButton;
		ui.Separator = &UiSeparator;
		ui.Checkbox = &UiCheckbox;
		ui.InvokeCommand = &UiInvokeCommand;

		RegisterPanelRootNode(state);
		const int result = (draw)(userData, &ui, &state);
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
