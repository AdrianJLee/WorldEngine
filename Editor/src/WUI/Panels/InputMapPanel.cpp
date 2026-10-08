#include "wldpch.h"
#include "WUI/Panels/InputMapPanel.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Gameplay/Framework/InputGlyphs.h"
#include "World/Gameplay/Framework/InputRemap.h"


namespace World
{
	namespace
	{
		std::string BindingLabel(const Gameplay::InputBinding& binding)
		{
			return Gameplay::InputGlyphs::GetGlyphText(binding.Device, binding.Code);
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
			// P1c-W3.6:绑定列文字走库件 `Wui::Label`(文字命令逐字段等价)。
			Wui::Label(ctx, { rect.X + rect.W - 180, y + 4 }, bindingText, theme.Text, 13.0f);
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
				// 捕获直接读**本帧真实按下的键**(WuiInputState::KeyPressed,引擎 KeyCodes = GLFW 空间;
				// 同一个向量就是编辑器文本/快捷键用的那一份),而不是一份硬编码候选表。
				//
				// M39:此前那份 `std::array<int, 62>` 候选表把 **VK 码**混进了 GLFW 码空间 ——
				// 9(VK Tab,GLFW 里是非法值)/13(VK Enter,GLFW Enter = 257)/16-18(VK 修饰键,
				// GLFW 用 340-347)/37-40(VK 方向键,GLFW 是 262-265)/96-105(VK 小键盘,
				// GLFW 是 320-329)/112-116(VK F1-F5,GLFW 是 290-294)。这些键**永远匹配不到**,
				// 用户看到的就是"按了没反应";而标点、F6-F25、Delete/Home/End/PageUp/PageDown、
				// Backspace、右侧修饰键压根不在表里 —— 这就是"很多键位录不上"。
				// 顺带修掉旧表 `std::array<int, 62>` 只给了 61 个初值、末位被零初始化的越界读。
				//
				// 用 KeyPressed(本帧沿)而不是 KeyDown(按住),一次按下只录一个键,长按不会反复覆盖。
				// Escape 上面已作为"取消"消费掉,这里不会再落到它。
				for (const uint32_t code : ctx.Input().KeyPressed)
				{
					const int keyCode = static_cast<int>(code);
					bool bound = false;
					for (Gameplay::InputAction& action : m_Map.Actions())
					{
						if (action.Name != m_RebindingAction)
							continue;
						action.Bindings.clear();
						action.Bindings.push_back({ Gameplay::InputDevice::Key, keyCode });
						bound = true;
					}
					if (!bound)
						continue;
					m_Status = "bound '" + m_RebindingAction + "' to key " + std::to_string(keyCode);
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

		// P1c-W3.6:状态行 + 底部提示同理(两条都只是 `{ Text, {x,y,0,0}, color, 0, 1, text, size, false }`)。
		Wui::Label(ctx, { rect.X + 8, y }, m_Status, theme.TextMuted, 13.0f);
		Wui::Label(ctx, { rect.X + 8, y + 20 },
			"click an action to rebind; Esc cancels; saved to assets/input.weinput",
			theme.TextMuted, 12.0f);
	}
}
