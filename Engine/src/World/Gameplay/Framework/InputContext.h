#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputTypes.h"
#include "World/Gameplay/Framework/InputModifier.h"
#include "World/Gameplay/Framework/InputTrigger.h"

#include <string>
#include <vector>

namespace World::Gameplay
{
	struct ActionBindingConfig
	{
		NameId Action;
		std::string ActionName;
		InputActionValueType ValueType = InputActionValueType::Button;
		InputBinding Binding;
		std::vector<InputModifier> Modifiers;
		std::vector<InputTrigger> Triggers;
	};

	class WLD_API InputMappingContext
	{
	public:
		InputMappingContext() = default;
		explicit InputMappingContext(const std::string& name, int32_t priority = 0, bool consume = true);

		NameId GetId() const { return m_Id; }
		const std::string& GetName() const { return m_Name; }
		int32_t GetPriority() const { return m_Priority; }
		void SetPriority(int32_t p) { m_Priority = p; }
		bool ConsumesInput() const { return m_ConsumeInput; }
		void SetConsumeInput(bool consume) { m_ConsumeInput = consume; }

		// R1:标记为"基础上下文" —— 装载时自动压栈(按 priority 参与解算),不需要游戏代码显式 Push。
		// 用于"任何时候都该生效"的映射集(例:步行/通用操作)。状态类上下文(载具/瞄准/菜单)
		// 不标这个位,由游戏代码或脚本按需 PushContext/PopContext。
		bool IsAutoPush() const { return m_AutoPush; }
		void SetAutoPush(bool autoPush) { m_AutoPush = autoPush; }

		void AddMapping(ActionBindingConfig config);
		const std::vector<ActionBindingConfig>& GetMappings() const { return m_Mappings; }

		std::vector<const ActionBindingConfig*> FindMappings(NameId action) const;

	private:
		NameId m_Id;
		std::string m_Name;
		int32_t m_Priority = 0;
		bool m_ConsumeInput = true;
		bool m_AutoPush = false;
		std::vector<ActionBindingConfig> m_Mappings;
	};
}
