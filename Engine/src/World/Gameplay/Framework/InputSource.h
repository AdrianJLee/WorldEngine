#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputTypes.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace World::Gameplay
{
	class WLD_API IInputSource
	{
	public:
		virtual ~IInputSource() = default;
		virtual void Poll(uint32_t player, RawInputState& outState, float dt) = 0;
	};

	// 硬件物理输入源
	class WLD_API DeviceInputSource : public IInputSource
	{
	public:
		void Poll(uint32_t player, RawInputState& outState, float dt) override;
	};

	// AI 注入输入源
	struct InjectedAction
	{
		NameId Action;
		float Value = 1.0f;
		uint32_t RemainingFrames = 1;
	};

	class WLD_API AiInjectionInputSource : public IInputSource
	{
	public:
		void InjectAction(uint32_t player, NameId action, float value, uint32_t frames);
		void Clear(uint32_t player);
		void Poll(uint32_t player, RawInputState& outState, float dt) override;

		bool HasPendingInjections(uint32_t player) const;

	private:
		std::unordered_map<uint32_t, std::vector<InjectedAction>> m_Injections;
	};
}
