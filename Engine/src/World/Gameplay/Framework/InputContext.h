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

		void AddMapping(ActionBindingConfig config);
		const std::vector<ActionBindingConfig>& GetMappings() const { return m_Mappings; }

		std::vector<const ActionBindingConfig*> FindMappings(NameId action) const;

	private:
		NameId m_Id;
		std::string m_Name;
		int32_t m_Priority = 0;
		bool m_ConsumeInput = true;
		std::vector<ActionBindingConfig> m_Mappings;
	};
}
