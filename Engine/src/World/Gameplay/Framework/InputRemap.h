#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputTypes.h"
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace World::Gameplay
{
	struct RemapConflict
	{
		NameId ConflictingAction;
		std::string ActionName;
		InputBinding Binding;
	};

	class WLD_API InputRemapManager
	{
	public:
		static InputRemapManager& Get();

		void SetOverride(NameId action, std::vector<InputBinding> bindings);
		const std::vector<InputBinding>* GetOverride(NameId action) const;
		void ClearOverride(NameId action);
		void ClearAllOverrides();

		bool HasConflict(InputBinding binding, NameId currentAction, RemapConflict* outConflict = nullptr) const;

		bool SaveOverrides(const std::filesystem::path& path, std::string* error = nullptr) const;
		bool LoadOverrides(const std::filesystem::path& path, std::string* error = nullptr);

	private:
		std::unordered_map<uint32_t, std::vector<InputBinding>> m_Overrides;
	};
}
