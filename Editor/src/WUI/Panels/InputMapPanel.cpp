#include "wldpch.h"
#include "WUI/Panels/InputMapPanel.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Gameplay/Framework/InputGlyphs.h"

#include <algorithm>
#include <vector>

namespace World
{
	namespace
	{
		constexpr float kRowH = 22.0f;
		constexpr float kGap = 2.0f;
		constexpr float kIndent = 14.0f;

		std::string BindingLabel(const Gameplay::InputBinding& binding)
		{
			return Gameplay::InputGlyphs::GetGlyphText(binding.Device, binding.Code);
		}

		const std::vector<std::string>& GamepadAxisOptions()
		{
			static const std::vector<std::string> options = { "(none)", "LX", "LY", "RX", "RY", "LT", "RT" };
			return options;
		}

		const std::vector<std::string>& TriggerTypeOptions()
		{
			static const std::vector<std::string> options = {
				"pressed", "released", "hold", "tap", "double_tap", "pulse", "chord" };
			return options;
		}

		const char* TriggerTypeName(Gameplay::TriggerType type)
		{
			switch (type)
			{
			case Gameplay::TriggerType::Pressed: return "pressed";
			case Gameplay::TriggerType::Released: return "released";
			case Gameplay::TriggerType::Hold: return "hold";
			case Gameplay::TriggerType::Tap: return "tap";
			case Gameplay::TriggerType::DoubleTap: return "double_tap";
			case Gameplay::TriggerType::Pulse: return "pulse";
			case Gameplay::TriggerType::Chord: return "chord";
			}
			return "pressed";
		}

		int TriggerTypeIndex(Gameplay::TriggerType type)
		{
			const std::vector<std::string>& options = TriggerTypeOptions();
			const std::string name = TriggerTypeName(type);
			const auto it = std::find(options.begin(), options.end(), name);
			return it == options.end() ? 0 : static_cast<int>(it - options.begin());
		}

		Gameplay::TriggerType TriggerTypeFromIndex(int index)
		{
			switch (index)
			{
			case 1: return Gameplay::TriggerType::Released;
			case 2: return Gameplay::TriggerType::Hold;
			case 3: return Gameplay::TriggerType::Tap;
			case 4: return Gameplay::TriggerType::DoubleTap;
			case 5: return Gameplay::TriggerType::Pulse;
			case 6: return Gameplay::TriggerType::Chord;
			default: return Gameplay::TriggerType::Pressed;
			}
		}

		// 该动作的第一个可用"空槽"设备码:同一个动作上重复加同一个键没有意义,
		// 所以按钮按"下一次点会加什么"轮转,而不是每次都加同一个。
		Gameplay::InputBinding NextBindingFor(const Gameplay::InputAction& action, Gameplay::InputDevice device,
			const int* candidates, std::size_t candidateCount)
		{
			for (std::size_t i = 0; i < candidateCount; ++i)
			{
				const bool used = std::any_of(action.Bindings.begin(), action.Bindings.end(),
					[device, &candidates, i](const Gameplay::InputBinding& existing)
					{
						return existing.Device == device && existing.Code == candidates[i];
					});
				if (!used)
					return Gameplay::InputBinding{ device, candidates[i] };
			}
			return Gameplay::InputBinding{ device, candidates[0] };
		}

		Gameplay::InputAction* FindAction(Gameplay::InputMap& map, const std::string& name)
		{
			for (Gameplay::InputAction& action : map.Actions())
				if (action.Name == name)
					return &action;
			return nullptr;
		}
	}

	void InputMapPanel::EnsureLoaded()
	{
		if (m_Loaded)
			return;
		m_Loaded = true;

		std::filesystem::path manifestPath;
		if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			std::string error;
			World::Asset::ProjectManifest manifest;
			if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
				m_Path = manifest.ResolveContentRoot(manifestPath) / "input.weinput";
		}

		if (m_Path.empty())
		{
			m_Status = "content root not found; open a project first";
			return;
		}

		std::string loadError;
		if (Gameplay::InputMap::Load(m_Path, &m_Map, &loadError))
		{
			m_Status = "loaded: " + m_Path.filename().string();
			return;
		}

		// 文件不存在/解析失败:B7 —— **不要**擅自写一份默认映射进用户的工程。
		// 以前这里会静默造出 Jump/MoveRight/MoveLeft 三件套并立刻落盘,于是"打开面板"
		// 本身就改动了工程内容。空模型 + 明确提示,让用户用下面的"新增"自己建。
		m_Status = "no input map at assets/input.weinput (use the Add rows below to build one)";
	}

	bool InputMapPanel::Save()
	{
		if (m_Path.empty())
		{
			m_Status = "content root not found; cannot save";
			return false;
		}
		std::string error;
		if (!Gameplay::InputMap::Save(m_Path, m_Map, &error))
		{
			m_Status = "save failed: " + error;
			return false;
		}
		m_Status = "saved: " + m_Path.filename().string();
		return true;
	}

	void InputMapPanel::MarkDirtyAndSave(const std::string& what)
	{
		m_Status = what;
		Save();
	}

	float InputMapPanel::ContentHeight() const
	{
		float height = 34.0f;                                    // 标题
		height += kRowH + kGap;                                  // 新增动作行
		height += (kRowH + kGap) * static_cast<float>(m_Map.Actions().size());
		height += 8.0f + kRowH;                                  // 轴标题
		for (const Gameplay::InputAxis& axis : m_Map.Axes())
			height += kRowH + kGap;
		height += kRowH * 2.0f + kGap;                           // 新增轴(名称 + 说明行)
		height += 8.0f + kRowH;                                  // 上下文标题
		for (const Gameplay::InputMappingContext& context : m_Map.Contexts())
		{
			height += kRowH + kGap;
			for (const Gameplay::ActionBindingConfig& mapping : context.GetMappings())
			{
				height += kRowH + kGap;
				height += (kRowH + kGap) * static_cast<float>(mapping.Triggers.size());
				height += kRowH;                                 // "+ trigger" 行
			}
			height += kRowH;                                     // "+ mapping" 行
		}
		height += kRowH * 2.0f + kGap;                           // 新增上下文
		height += 60.0f;                                         // 保存 + 状态
		return height;
	}

	void InputMapPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		EnsureLoaded();

		Wui::PanelBackground(ctx, rect, { 0.10f, 0.105f, 0.115f, 1 });
		Wui::SectionHeader(ctx, { rect.X + 8, rect.Y + 6, rect.W - 16, 20 }, "Input Map", theme.Accent, theme);

		const Wui::WuiRect viewport { rect.X, rect.Y + 30, rect.W, rect.H - 30 };
		const float contentHeight = ContentHeight();
		Wui::BeginScrollArea(ctx, viewport, contentHeight, m_ScrollY, theme, Wui::HashId("input.scroll"));
		// 滚动区契约(与内容浏览器/组件选择器同一口径):调用方自己按 -scrollY 偏移内容原点,
		// 用 ctx.ClipAllows 跳过滚出视口的行(用户看不到的行也不该被 AI 点到)。
		const Wui::WuiRect content { viewport.X, viewport.Y - m_ScrollY, viewport.W, contentHeight };

		const float left = content.X + 10;
		const float width = std::max(120.0f, content.W - 20);
		const float actionCol = std::min(190.0f, width * 0.34f);
		float y = content.Y + 6;

		// ---------------- 新增动作 ----------------
		const float buttonW = 64.0f;
		const float fieldW = std::max(80.0f, width - buttonW - 8);
		Wui::TextField(ctx, Wui::HashId("input.action.new.name"),
			{ left, y + 1, fieldW, kRowH - 2 }, m_NewActionName, theme);
		if (Wui::ButtonEx(ctx, Wui::HashId("input.action.new.add"),
			{ left + fieldW + 8, y, buttonW, kRowH }, "Add", theme, !m_NewActionName.empty(), true) ||
			(ctx.IsKeyPressed(KeyCodes::Enter) && !m_NewActionName.empty()))
		{
			if (FindAction(m_Map, m_NewActionName))
			{
				m_Status = "action '" + m_NewActionName + "' already exists";
			}
			else
			{
				Gameplay::InputAction created;
				created.Name = m_NewActionName;
				m_Map.Actions().push_back(created);
				MarkDirtyAndSave("added action '" + created.Name + "'");
				m_NewActionName.clear();
			}
		}
		Wui::Label(ctx, { left + fieldW + 8 + buttonW + 8, y + 4 },
			"new action name, then Add", theme.TextMuted, 12.0f);
		y += kRowH + kGap;

		// ---------------- 动作表 ----------------
		for (std::size_t index = 0; index < m_Map.Actions().size();)
		{
			Gameplay::InputAction& action = m_Map.Actions()[index];
			const std::string idBase = "input.action." + action.Name;

			const bool rebinding = m_RebindingAction == action.Name;
			const bool appending = m_AppendKeyAction == action.Name;
			const std::string label = action.Name
				+ (rebinding ? "  [press any key to replace...]" : (appending ? "  [press any key to add...]" : ""));
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".rebind").c_str()),
				{ left, y, actionCol, kRowH }, label, theme, true, rebinding))
			{
				m_AppendKeyAction.clear();
				m_RebindingAction = rebinding ? std::string() : action.Name;
				m_Status = m_RebindingAction.empty()
					? "rebind cancelled"
					: "press any key to replace every binding of '" + action.Name + "'";
			}

			const float x0 = left + actionCol + 8;
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".addkey").c_str()),
				{ x0, y, 46.0f, kRowH }, "+Key", theme, true, appending))
			{
				m_RebindingAction.clear();
				m_AppendKeyAction = appending ? std::string() : action.Name;
				m_Status = m_AppendKeyAction.empty()
					? "add-key cancelled"
					: "press any key to ADD a binding to '" + action.Name + "'";
			}
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".addmouse").c_str()),
				{ x0 + 50.0f, y, 58.0f, kRowH }, "+Mouse", theme))
			{
				static const int mouseButtons[] = { 0, 1, 2 };
				action.Bindings.push_back(NextBindingFor(action, Gameplay::InputDevice::Mouse,
					mouseButtons, 3));
				MarkDirtyAndSave("added mouse binding to '" + action.Name + "'");
			}
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".addpad").c_str()),
				{ x0 + 112.0f, y, 50.0f, kRowH }, "+Pad", theme))
			{
				static const int padButtons[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
				action.Bindings.push_back(NextBindingFor(action, Gameplay::InputDevice::Gamepad,
					padButtons, 8));
				MarkDirtyAndSave("added gamepad binding to '" + action.Name + "'");
			}

			// 绑定列表:每条一个可点的 [X](点掉即删),其余用文字列出。
			float bx = x0 + 168.0f;
			const float removeW = 18.0f;
			bool removed = false;
			for (std::size_t bindingIndex = 0; bindingIndex < action.Bindings.size(); ++bindingIndex)
			{
				const std::string removeId = idBase + ".binding." + std::to_string(bindingIndex) + ".remove";
				const std::string glyph = BindingLabel(action.Bindings[bindingIndex]);
				const float labelW = std::min(96.0f, 12.0f + 8.0f * static_cast<float>(glyph.size()));
				if (Wui::ButtonEx(ctx, Wui::HashId(removeId.c_str()),
					{ bx, y, removeW, kRowH }, "x", theme))
				{
					action.Bindings.erase(action.Bindings.begin() + static_cast<std::ptrdiff_t>(bindingIndex));
					removed = true;
					break;
				}
				Wui::Label(ctx, { bx + removeW + 4, y + 4 }, glyph, theme.Text, 12.5f);
				bx += removeW + 4 + labelW;
			}
			if (removed)
			{
				MarkDirtyAndSave("removed a binding from '" + action.Name + "'");
				continue;                                        // 容器已变,本帧不再用该引用
			}
			if (action.Bindings.empty())
				Wui::Label(ctx, { bx, y + 4 }, "(unbound)", theme.TextMuted, 12.5f);

			const float delW = 26.0f;
			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".delete").c_str()),
				{ content.X + content.W - 10 - delW, y, delW, kRowH }, "Del", theme))
			{
				m_Map.Actions().erase(m_Map.Actions().begin() + static_cast<std::ptrdiff_t>(index));
				MarkDirtyAndSave("deleted action '" + action.Name + "'");
				continue;
			}
			++index;
			y += kRowH + kGap;
		}

		// ---------------- 轴 ----------------
		y += 8.0f;
		Wui::Label(ctx, { left, y }, "Axes", theme.Accent, 13.0f);
		y += kRowH;

		std::vector<std::string> actionOptions;
		actionOptions.push_back("(none)");
		for (const Gameplay::InputAction& action : m_Map.Actions())
			actionOptions.push_back(action.Name);

		for (std::size_t index = 0; index < m_Map.Axes().size();)
		{
			Gameplay::InputAxis& axis = m_Map.Axes()[index];
			const std::string idBase = "input.axis." + axis.Name;
			bool changed = false;

			Wui::Label(ctx, { left, y + 4 }, axis.Name, theme.Text, 12.5f);
			const float x0 = left + actionCol;
			int positive = 0;
			{
				const auto it = std::find(actionOptions.begin(), actionOptions.end(), axis.PositiveAction);
				positive = it == actionOptions.end() ? 0 : static_cast<int>(it - actionOptions.begin());
			}
			const int positiveBefore = positive;
			Wui::Combo(ctx, Wui::HashId((idBase + ".positive").c_str()),
				{ x0, y, 120.0f, kRowH }, "positive", actionOptions, positive, theme);
			if (positive != positiveBefore)
			{
				axis.PositiveAction = positive == 0 ? std::string() : actionOptions[static_cast<std::size_t>(positive)];
				changed = true;
			}

			int negative = 0;
			{
				const auto it = std::find(actionOptions.begin(), actionOptions.end(), axis.NegativeAction);
				negative = it == actionOptions.end() ? 0 : static_cast<int>(it - actionOptions.begin());
			}
			const int negativeBefore = negative;
			Wui::Combo(ctx, Wui::HashId((idBase + ".negative").c_str()),
				{ x0 + 124.0f, y, 120.0f, kRowH }, "negative", actionOptions, negative, theme);
			if (negative != negativeBefore)
			{
				axis.NegativeAction = negative == 0 ? std::string() : actionOptions[static_cast<std::size_t>(negative)];
				changed = true;
			}

			int gamepad = 0;
			{
				const auto it = std::find(GamepadAxisOptions().begin(), GamepadAxisOptions().end(), axis.GamepadAxis);
				gamepad = it == GamepadAxisOptions().end() ? 0 : static_cast<int>(it - GamepadAxisOptions().begin());
			}
			const int gamepadBefore = gamepad;
			Wui::Combo(ctx, Wui::HashId((idBase + ".gamepad").c_str()),
				{ x0 + 248.0f, y, 76.0f, kRowH }, "gamepad", GamepadAxisOptions(), gamepad, theme);
			if (gamepad != gamepadBefore)
			{
				axis.GamepadAxis = gamepad == 0 ? std::string()
					: GamepadAxisOptions()[static_cast<std::size_t>(gamepad)];
				changed = true;
			}

			if (Wui::DragFloat(ctx, Wui::HashId((idBase + ".deadzone").c_str()),
				{ x0 + 328.0f, y, 70.0f, kRowH }, axis.DeadZone, 0.005f, 0.0f, 0.95f, theme))
				changed = true;

			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".delete").c_str()),
				{ content.X + content.W - 10 - 26.0f, y, 26.0f, kRowH }, "Del", theme))
			{
				const std::string name = axis.Name;
				m_Map.Axes().erase(m_Map.Axes().begin() + static_cast<std::ptrdiff_t>(index));
				MarkDirtyAndSave("deleted axis '" + name + "'");
				continue;
			}
			if (changed)
				MarkDirtyAndSave("updated axis '" + axis.Name + "'");
			++index;
			y += kRowH + kGap;
		}

		// 新增轴:名字来自一个文本框,正负动作先用第一/第二个动作兜底,随后在上表里改。
		Wui::TextField(ctx, Wui::HashId("input.axis.new.name"),
			{ left, y + 1, 160.0f, kRowH - 2 }, m_NewAxisName, theme);
		if (Wui::ButtonEx(ctx, Wui::HashId("input.axis.new.add"),
			{ left + 168.0f, y, 64.0f, kRowH }, "Add Axis", theme, !m_NewAxisName.empty()))
		{
			const bool exists = std::any_of(m_Map.Axes().begin(), m_Map.Axes().end(),
				[this](const Gameplay::InputAxis& existing) { return existing.Name == m_NewAxisName; });
			if (exists)
			{
				m_Status = "axis '" + m_NewAxisName + "' already exists";
			}
			else
			{
				Gameplay::InputAxis created;
				created.Name = m_NewAxisName;
				m_Map.Axes().push_back(created);
				MarkDirtyAndSave("added axis '" + created.Name + "'");
				m_NewAxisName.clear();
			}
		}
		y += kRowH * 2.0f + kGap;

		// ---------------- 上下文 ----------------
		Wui::Label(ctx, { left, y }, "Contexts (default = auto-activated on load)", theme.Accent, 13.0f);
		y += kRowH;

		for (std::size_t index = 0; index < m_Map.Contexts().size();)
		{
			Gameplay::InputMappingContext& context = m_Map.Contexts()[index];
			const std::string idBase = "input.context." + std::to_string(index);
			bool changed = false;

			Wui::Label(ctx, { left, y + 4 }, context.GetName(), theme.Text, 12.5f);
			const float x0 = left + actionCol;

			int64_t priority = context.GetPriority();
			if (Wui::DragInt(ctx, Wui::HashId((idBase + ".priority").c_str()),
				{ x0, y, 56.0f, kRowH }, priority, -100, 100, theme))
			{
				context.SetPriority(static_cast<int32_t>(priority));
				changed = true;
			}

			bool consume = context.ConsumesInput();
			if (Wui::Checkbox(ctx, Wui::HashId((idBase + ".consume").c_str()),
				{ x0 + 62.0f, y, 96.0f, kRowH }, "consume", consume, theme))
			{
				context.SetConsumeInput(consume);
				changed = true;
			}

			bool autoPush = context.IsAutoPush();
			if (Wui::Checkbox(ctx, Wui::HashId((idBase + ".default").c_str()),
				{ x0 + 164.0f, y, 80.0f, kRowH }, "default", autoPush, theme))
			{
				context.SetAutoPush(autoPush);
				changed = true;
			}

			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".delete").c_str()),
				{ content.X + content.W - 10 - 26.0f, y, 26.0f, kRowH }, "Del", theme))
			{
				const std::string name = context.GetName();
				m_Map.Contexts().erase(m_Map.Contexts().begin() + static_cast<std::ptrdiff_t>(index));
				MarkDirtyAndSave("deleted context '" + name + "'");
				continue;
			}
			if (changed)
				MarkDirtyAndSave("updated context '" + context.GetName() + "'");
			y += kRowH + kGap;

			std::vector<Gameplay::ActionBindingConfig>& mappings = context.Mappings();
			bool mappingRemoved = false;
			for (std::size_t mappingIndex = 0; mappingIndex < mappings.size(); ++mappingIndex)
			{
				Gameplay::ActionBindingConfig& mapping = mappings[mappingIndex];
				const std::string mappingIdBase = idBase + ".mapping." + std::to_string(mappingIndex);
				const float indent = left + kIndent;

				int actionIndex = 0;
				{
					const auto it = std::find(actionOptions.begin(), actionOptions.end(), mapping.ActionName);
					actionIndex = it == actionOptions.end() ? 0 : static_cast<int>(it - actionOptions.begin());
				}
				const int actionIndexBefore = actionIndex;
				Wui::Combo(ctx, Wui::HashId((mappingIdBase + ".action").c_str()),
					{ indent, y, 130.0f, kRowH }, "action", actionOptions, actionIndex, theme);
				if (actionIndex != actionIndexBefore)
				{
					mapping.ActionName = actionIndex == 0 ? std::string()
						: actionOptions[static_cast<std::size_t>(actionIndex)];
					mapping.Action = StringPool::Get().InternName(mapping.ActionName);
					MarkDirtyAndSave("retargeted a mapping in '" + context.GetName() + "'");
				}

				Wui::Label(ctx, { indent + 138.0f, y + 4 }, BindingLabel(mapping.Binding), theme.Text, 12.5f);
				Wui::Label(ctx, { indent + 250.0f, y + 4 },
					"(" + std::to_string(mapping.Triggers.size()) + " trigger)", theme.TextMuted, 11.5f);

				if (Wui::ButtonEx(ctx, Wui::HashId((mappingIdBase + ".remove").c_str()),
					{ content.X + content.W - 10 - 26.0f, y, 26.0f, kRowH }, "Del", theme))
				{
					mappings.erase(mappings.begin() + static_cast<std::ptrdiff_t>(mappingIndex));
					MarkDirtyAndSave("removed a mapping from '" + context.GetName() + "'");
					mappingRemoved = true;
					break;
				}
				y += kRowH + kGap;

				// 触发器:每条一行(类型可换 + 时长/间隔可调 + 删)。
				bool triggerRemoved = false;
				for (std::size_t triggerIndex = 0; triggerIndex < mapping.Triggers.size(); ++triggerIndex)
				{
					Gameplay::InputTrigger& trigger = mapping.Triggers[triggerIndex];
					const std::string triggerIdBase = mappingIdBase + ".trigger." + std::to_string(triggerIndex);
					const float triggerIndent = indent + kIndent;
					bool triggerChanged = false;

					int typeIndex = TriggerTypeIndex(trigger.Type);
					const int typeIndexBefore = typeIndex;
					Wui::Combo(ctx, Wui::HashId((triggerIdBase + ".type").c_str()),
						{ triggerIndent, y, 110.0f, kRowH }, "type", TriggerTypeOptions(), typeIndex, theme);
					if (typeIndex != typeIndexBefore)
					{
						trigger.Type = TriggerTypeFromIndex(typeIndex);
						triggerChanged = true;
						MarkDirtyAndSave("changed a trigger type in '" + context.GetName() + "'");
					}

					// 只显示该类型真正消费的参数(与 Load/Save 的逐字段口径一致)。
					float paramX = triggerIndent + 118.0f;
					const auto showParam = [&](const char* suffix, const char* label, float& value)
					{
						if (Wui::DragFloat(ctx, Wui::HashId((triggerIdBase + "." + suffix).c_str()),
							{ paramX, y, 74.0f, kRowH }, value, 0.005f, 0.0f, 2.0f, theme))
							triggerChanged = true;
						Wui::Label(ctx, { paramX + 78.0f, y + 4 }, label, theme.TextMuted, 11.0f);
						paramX += 78.0f + 22.0f;
					};
					switch (trigger.Type)
					{
					case Gameplay::TriggerType::Hold:
					case Gameplay::TriggerType::Tap:
						showParam("duration", "duration", trigger.Duration);
						break;
					case Gameplay::TriggerType::DoubleTap:
						showParam("interval", "interval", trigger.Interval);
						showParam("duration", "duration", trigger.Duration);
						break;
					case Gameplay::TriggerType::Pulse:
						showParam("interval", "interval", trigger.Interval);
						break;
					default:
						break;
					}
					if (triggerChanged && m_Status.rfind("changed", 0) != 0)
						MarkDirtyAndSave("updated a trigger in '" + context.GetName() + "'");

					if (Wui::ButtonEx(ctx, Wui::HashId((triggerIdBase + ".remove").c_str()),
						{ content.X + content.W - 10 - 26.0f, y, 26.0f, kRowH }, "Del", theme))
					{
						mapping.Triggers.erase(mapping.Triggers.begin() + static_cast<std::ptrdiff_t>(triggerIndex));
						MarkDirtyAndSave("removed a trigger from '" + context.GetName() + "'");
						triggerRemoved = true;
						break;
					}
					y += kRowH + kGap;
				}
				if (triggerRemoved)
				{
					--mappingIndex;
					continue;
				}

				if (Wui::ButtonEx(ctx, Wui::HashId((mappingIdBase + ".addtrigger").c_str()),
					{ indent + kIndent, y, 96.0f, kRowH }, "+ Trigger", theme, !mapping.ActionName.empty()))
				{
					mapping.Triggers.push_back(Gameplay::InputTrigger::MakePressed());
					MarkDirtyAndSave("added a trigger to '" + context.GetName() + "'");
				}
				y += kRowH;
			}
			if (mappingRemoved)
			{
				continue;                                        // 上下文容器已变
			}

			if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".addmapping").c_str()),
				{ left + kIndent, y, 110.0f, kRowH }, "+ Mapping", theme, m_Map.Actions().size() > 0))
			{
				Gameplay::ActionBindingConfig created;
				created.ActionName = m_Map.Actions().front().Name;
				created.Action = m_Map.Actions().front().Id;
				created.Binding = { Gameplay::InputDevice::Key, KeyCodes::Space };
				mappings.push_back(created);
				MarkDirtyAndSave("added a mapping to '" + context.GetName() + "'");
			}
			y += kRowH;
		}

		// 新增上下文:名字 + Add;默认 consume=false / default=true(基础上下文是最常见的入口)。
		Wui::TextField(ctx, Wui::HashId("input.context.new.name"),
			{ left, y + 1, 160.0f, kRowH - 2 }, m_NewContextName, theme);
		if (Wui::ButtonEx(ctx, Wui::HashId("input.context.new.add"),
			{ left + 168.0f, y, 96.0f, kRowH }, "Add Context", theme, !m_NewContextName.empty()))
		{
			Gameplay::InputMappingContext created(m_NewContextName, 0, false);
			created.SetAutoPush(true);
			m_Map.Contexts().push_back(std::move(created));
			MarkDirtyAndSave("added context '" + m_NewContextName + "'");
			m_NewContextName.clear();
		}
		y += kRowH * 2.0f + kGap;

		// ---------------- 保存 + 状态 ----------------
		if (Wui::ButtonEx(ctx, Wui::HashId("input.save"),
			{ left, y, 150.0f, kRowH }, "Save Input Map", theme, !m_Path.empty(), true))
			Save();
		y += kRowH + 6.0f;
		Wui::Label(ctx, { left, y }, m_Status, theme.TextMuted, 12.5f);
		Wui::Label(ctx, { left, y + 18 }, m_Path.empty() ? std::string() : m_Path.filename().string(),
			theme.TextMuted, 11.5f);

		Wui::EndScrollArea(ctx);

		// ---------------- 按键捕获 ----------------
		// 捕获读**本帧真实按下的键**(`WuiInputState::KeyPressed`,引擎 KeyCodes = GLFW 空间;
		// 与编辑器文本/快捷键同一份数据),不用候选表 —— 任意键都能录,键码空间天然正确。
		// 用 KeyPressed(本帧沿)而不是按住状态:一次按下只录一个键,长按不会反复覆盖。
		// M39 的教训:此前那份硬编码候选表把 VK 码混进了 GLFW 码空间,其中 25 项恒不匹配、
		// 其余常用键压根不在表内 —— 表现就是"很多键位录不上"。
		if (!m_RebindingAction.empty() || !m_AppendKeyAction.empty())
		{
			const bool replacing = !m_RebindingAction.empty();
			const std::string target = replacing ? m_RebindingAction : m_AppendKeyAction;
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				m_RebindingAction.clear();
				m_AppendKeyAction.clear();
				m_Status = "capture cancelled (Esc)";
			}
			else
			{
				for (const uint32_t code : ctx.Input().KeyPressed)
				{
					Gameplay::InputAction* action = FindAction(m_Map, target);
					if (!action)
						break;
					const int keyCode = static_cast<int>(code);
					if (replacing)
					{
						action->Bindings.clear();
						action->Bindings.push_back({ Gameplay::InputDevice::Key, keyCode });
					}
					else
					{
						action->Bindings.push_back({ Gameplay::InputDevice::Key, keyCode });
					}
					m_RebindingAction.clear();
					m_AppendKeyAction.clear();
					MarkDirtyAndSave(std::string(replacing ? "replaced" : "added") + " binding for '" + target
						+ "' = key " + std::to_string(keyCode));
					break;
				}
			}
		}
	}
}
