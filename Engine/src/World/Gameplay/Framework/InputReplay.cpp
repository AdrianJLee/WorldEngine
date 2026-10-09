#include "wldpch.h"
#include "World/Gameplay/Framework/InputReplay.h"
#include "World/Gameplay/Framework/InputMap.h"
#include "World/Gameplay/Framework/InputRemap.h"
#include "World/Gameplay/Framework/GamepadBackend.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Core/Application.h"
#include "World/Core/Input.h"

#ifdef WLD_PLATFORM_WINDOWS
#include "World/Platform/Windows/WindowsRawInput.h"
#endif

#include <fstream>
#include <algorithm>

namespace World::Gameplay
{
	void DeviceInputSource::Poll(uint32_t player, RawInputState& outState, float dt)
	{
		(void)dt;
		if (player != 0)
			return;

		if (!Application::HasInstance())
			return;

		// 1. 键鼠状态采样 (遍历 actions 自带绑定、重映射覆盖与全部活跃 contexts)
		if (GameApp* app = GameApp::TryGet())
		{
			const InputMap& map = app->Input().GetMap();
			for (const InputAction& action : map.Actions())
			{
				const auto* overrides = InputRemapManager::Get().GetOverride(action.Id);
				const auto& bindings = (overrides && !overrides->empty()) ? *overrides : action.Bindings;
				for (const InputBinding& b : bindings)
				{
					if (b.Device == InputDevice::Key)
					{
						outState.SetKey(b.Code, Input::IsKeyPressed(b.Code));
					}
					else if (b.Device == InputDevice::Mouse)
					{
						outState.SetMouseButton(b.Code, !m_PointerCaptured && Input::IsMouseButtonPressed(b.Code));
					}
				}
			}
			for (const InputMappingContext& ctx : map.Contexts())
			{
				for (const ActionBindingConfig& cfg : ctx.GetMappings())
				{
					if (cfg.Binding.Device == InputDevice::Key)
					{
						outState.SetKey(cfg.Binding.Code, Input::IsKeyPressed(cfg.Binding.Code));
					}
					else if (cfg.Binding.Device == InputDevice::Mouse)
					{
						outState.SetMouseButton(cfg.Binding.Code, !m_PointerCaptured && Input::IsMouseButtonPressed(cfg.Binding.Code));
					}
				}
			}
		}

		// 2. 指针位置与滚轮
		const auto pos = Input::GetMousePosition();
		outState.MousePosition = glm::vec2(pos.first, pos.second);
		if (!m_PointerCaptured)
		{
			const auto scroll = Input::GetScrollDelta();
			outState.ScrollDelta = glm::vec2(scroll.first, scroll.second);
		}
		else
		{
			outState.ScrollDelta = glm::vec2(0.0f);
		}

#ifdef WLD_PLATFORM_WINDOWS
		const int rawX = Platform::WindowsRawInput::GetAccumulatedRawX();
		const int rawY = Platform::WindowsRawInput::GetAccumulatedRawY();
		Platform::WindowsRawInput::ResetAccumulated();
		if (rawX != 0 || rawY != 0)
		{
			outState.MouseDelta = glm::vec2(static_cast<float>(rawX), static_cast<float>(rawY));
		}
#endif

		// 3. 4 个手柄槽位
		for (uint32_t slot = 0; slot < 4; ++slot)
		{
			GamepadState gpState;
			if (GamepadBackend::Get().Poll(slot, gpState))
			{
				outState.GamepadButtons[slot] = gpState.Buttons;
				for (int a = 0; a < 6; ++a)
					outState.GamepadAxes[slot][a] = gpState.Axes[a];
				outState.GamepadConnected[slot] = true;
			}
			else
			{
				outState.GamepadConnected[slot] = false;
			}
		}
	}

	void AiInjectionInputSource::InjectAction(uint32_t player, NameId action, float value, uint32_t frames)
	{
		m_Injections[player].push_back({ action, value, frames > 0 ? frames : 1 });
	}

	void AiInjectionInputSource::Clear(uint32_t player)
	{
		m_Injections.erase(player);
	}

	bool AiInjectionInputSource::HasPendingInjections(uint32_t player) const
	{
		auto it = m_Injections.find(player);
		return it != m_Injections.end() && !it->second.empty();
	}

	void AiInjectionInputSource::Poll(uint32_t player, RawInputState& outState, float dt)
	{
		(void)dt;
		auto it = m_Injections.find(player);
		if (it == m_Injections.end())
			return;

		for (auto actionIt = it->second.begin(); actionIt != it->second.end(); )
		{
			if (actionIt->RemainingFrames > 0)
			{
				--actionIt->RemainingFrames;
				++actionIt;
			}
			else
			{
				actionIt = it->second.erase(actionIt);
			}
		}
	}

	bool InputReplay::Save(const std::filesystem::path& path, const ReplayHeader& header,
		const std::vector<InputFrame>& frames, std::string* error)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary);
		if (!out)
		{
			if (error) *error = "Failed to open replay file for write: " + path.string();
			return false;
		}

		ReplayHeader h = header;
		h.FrameCount = static_cast<uint32_t>(frames.size());
		out.write(reinterpret_cast<const char*>(&h), sizeof(ReplayHeader));
		if (!frames.empty())
		{
			out.write(reinterpret_cast<const char*>(frames.data()), frames.size() * sizeof(InputFrame));
		}
		if (error) error->clear();
		return true;
	}

	bool InputReplay::Load(const std::filesystem::path& path, ReplayHeader* outHeader,
		std::vector<InputFrame>* outFrames, std::string* error)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			if (error) *error = "Failed to open replay file for read: " + path.string();
			return false;
		}

		ReplayHeader h;
		in.read(reinterpret_cast<char*>(&h), sizeof(ReplayHeader));
		if (h.Magic != 0x50455257)
		{
			if (error) *error = "Invalid replay magic header";
			return false;
		}

		if (outHeader)
			*outHeader = h;

		if (outFrames)
		{
			outFrames->resize(h.FrameCount);
			if (h.FrameCount > 0)
			{
				in.read(reinterpret_cast<char*>(outFrames->data()), h.FrameCount * sizeof(InputFrame));
			}
		}
		if (error) error->clear();
		return true;
	}

	uint32_t InputReplay::ComputeStateHash(const std::vector<InputFrame>& frames)
	{
		// FNV-1a 32位哈希
		uint32_t hash = 2166136261u;
		const uint8_t* data = reinterpret_cast<const uint8_t*>(frames.data());
		const std::size_t size = frames.size() * sizeof(InputFrame);
		for (std::size_t i = 0; i < size; ++i)
		{
			hash ^= data[i];
			hash *= 16777619u;
		}
		return hash;
	}

	ReplayInputSource::ReplayInputSource(std::vector<InputFrame> frames)
		: m_Frames(std::move(frames)) {}

	void ReplayInputSource::Poll(uint32_t player, RawInputState& outState, float dt)
	{
		(void)dt;
		if (m_Cursor < m_Frames.size())
		{
			const InputFrame& f = m_Frames[m_Cursor];
			if (f.PlayerIndex == player)
			{
				outState.MousePosition.x = static_cast<float>(f.MouseX);
				outState.MousePosition.y = static_cast<float>(f.MouseY);
				outState.MouseDelta.x = static_cast<float>(f.MouseDeltaX);
				outState.MouseDelta.y = static_cast<float>(f.MouseDeltaY);
				outState.ScrollDelta.x = static_cast<float>(f.ScrollX);
				outState.ScrollDelta.y = static_cast<float>(f.ScrollY);
				if (GameApp* app = GameApp::TryGet())
				{
					app->Input().ApplyFrame(f);
				}
				++m_Cursor;
			}
		}
	}

	bool ReplayInputSource::IsFinished() const
	{
		return m_Cursor >= m_Frames.size();
	}
}
