#include "wldpch.h"
#include "InputMapPanel.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <array>

namespace World
{
	namespace
	{
		// 捕获用候选键:常用集合(字母/数字/修饰/方向/空格等)。Escape 用于取消捕获。
		const std::array<int, 62>& CaptureKeys()
		{
			static const std::array<int, 62> keys = {
				32, 9, 13, 16, 17, 18, 37, 38, 39, 40,
				48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
				65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86,
				87, 88, 89, 90,
				96, 97, 98, 99, 100, 101, 102, 103, 104, 105,
				112, 113, 114, 115, 116
			};
			return keys;
		}

		std::string BindingLabel(const Gameplay::InputBinding& binding)
		{
			const char* device = binding.Device == Gameplay::InputDevice::Mouse ? "mouse"
				: (binding.Device == Gameplay::InputDevice::Gamepad ? "pad" : "key");
			return std::string(device) + " " + std::to_string(binding.Code);
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

		std::string loadError;
		if (!m_Path.empty() && !Gameplay::InputMap::Load(m_Path, &m_Map, &loadError))
		{
			// 首次使用:给一份可用默认(空格跳跃 / WASD 轴),随后由 Save 落盘。
			Gameplay::InputAction jump;
			jump.Name = "Jump";
			jump.Bindings = { { Gameplay::InputDevice::Key, 32 } };
			Gameplay::InputAction right;
			right.Name = "MoveRight";
			right.Bindings = { { Gameplay::InputDevice::Key, 68 } };
			Gameplay::InputAction left;
			left.Name = "MoveLeft";
			left.Bindings = { { Gameplay::InputDevice::Key, 65 } };
			m_Map.Actions() = { jump, right, left };
			Gameplay::InputAxis move;
			move.Name = "Move";
			move.PositiveAction = "MoveRight";
			move.NegativeAction = "MoveLeft";
			move.GamepadAxis = "LX";
			m_Map.Axes() = { move };
			m_Status = "defaults created (not saved yet)";
			Save();
		}
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

	void InputMapPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		EnsureLoaded();

		Wui::PanelBackground(ctx, rect, { 0.10f, 0.105f, 0.115f, 1 });
		Wui::SectionHeader(ctx, { rect.X + 8, rect.Y + 6, rect.W - 16, 20 }, "Input Map", theme.Accent, theme);

		float y = rect.Y + 32;
		for (size_t i = 0; i < m_Map.Actions().size(); ++i)
		{
			Gameplay::InputAction& action = m_Map.Actions()[i];
			const std::string id = "input.action." + action.Name;
			const bool rebinding = m_RebindingAction == action.Name;
			const std::string label = action.Name + (rebinding ? "  [press any key...]" : "");
			if (Wui::ContextMenuItem(ctx, Wui::HashId(id.c_str()),
				{ rect.X + 8, y, rect.W - 200, 22 }, label, theme))
			{
				m_RebindingAction = rebinding ? std::string() : action.Name;
				m_Status = m_RebindingAction.empty() ? "rebind cancelled" : "press any key to bind '" + action.Name + "'";
			}

			const std::string bindingText = action.Bindings.empty() ? "(unbound)" : BindingLabel(action.Bindings[0]);
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
				{ rect.X + rect.W - 180, y + 4, 0, 0 }, theme.Text, 0, 1.0f, bindingText, 13.0f, false });
			y += 24;
		}

		// 捕获:捕获期间轮询候选键(捕获只影响本面板,其它面板仍会看到同一次按键:
		// 最小可用版的已知取舍,后续可加"输入独占"标志)。
		if (!m_RebindingAction.empty())
		{
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				m_RebindingAction.clear();
				m_Status = "rebind cancelled (Esc)";
			}
			else
			{
				for (const int code : CaptureKeys())
				{
					if (!ctx.IsKeyPressed(code))
						continue;
					for (Gameplay::InputAction& action : m_Map.Actions())
					{
						if (action.Name != m_RebindingAction)
							continue;
						action.Bindings.clear();
						action.Bindings.push_back({ Gameplay::InputDevice::Key, code });
					}
					m_Status = "bound '" + m_RebindingAction + "' to key " + std::to_string(code);
					m_RebindingAction.clear();
					Save();
					break;
				}
			}
		}

		y += 6;
		if (Wui::ContextMenuItem(ctx, Wui::HashId("input.save"),
			{ rect.X + 8, y, 160, 22 }, "Save Input Map", theme))
			Save();
		y += 26;

		ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
			{ rect.X + 8, y, 0, 0 }, theme.TextMuted, 0, 1.0f, m_Status, 13.0f, false });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
			{ rect.X + 8, y + 20, 0, 0 }, theme.TextMuted, 0, 1.0f,
			"click an action to rebind; Esc cancels; saved to assets/input.weinput", 12.0f, false });
	}
}
