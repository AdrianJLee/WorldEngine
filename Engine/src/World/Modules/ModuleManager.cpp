#include "wldpch.h"
#include "World/Modules/ModuleManager.h"
#include "World/Core/WorldContext.h"
#include "World/Utils/DynamicLibrary.h"

namespace World::Modules
{
	const char* ModuleManager::StatusName(Status status)
	{
		switch (status)
		{
			case Status::Ok: return "ok";
			case Status::NotFound: return "module file not found";
			case Status::LoadFailed: return "library load failed";
			case Status::MissingEntry: return "missing WeGameModuleQuery export";
			case Status::AbiMismatch: return "module ABI/struct mismatch";
			case Status::RegistrationFailed: return "module registration failed";
			default: return "unknown";
		}
	}

	ModuleManager::Status ModuleManager::Load(const std::filesystem::path& path, WorldContext& context, std::string* error)
	{
		if (!std::filesystem::exists(path))
		{
			if (error) *error = "module not found: " + path.string();
			return Status::NotFound;
		}

		auto library = std::make_unique<DynamicLibrary>();
		if (!library->Load(path.string()))
		{
			const std::string message = library->GetLastError();
			if (error) *error = message + " (" + path.string() + ")";
			return Status::LoadFailed;
		}

		using QueryFunc = const WeModule* (*)(uint32_t hostAbiVersion);
		const auto query = reinterpret_cast<QueryFunc>(library->GetSymbol("WeGameModuleQuery"));
		if (!query)
		{
			library->Unload();
			if (error) *error = "WeGameModuleQuery export missing in " + path.string();
			return Status::MissingEntry;
		}

		const WeModule* module = query(WE_MODULE_ABI_VERSION);
		// 等值校验(不是 >=):脚本组件重写改过 schema 传递结构(`Schema::ScriptBinding`)的形状,
		// 旧模块必须被拒绝而不是按新布局解释。
		if (!module || module->StructSize != sizeof(WeModule) || module->AbiVersion != WE_MODULE_ABI_VERSION || !module->Register)
		{
			library->Unload();
			if (error) *error = "module ABI/struct mismatch in " + path.string();
			return Status::AbiMismatch;
		}

		if (!module->Register(context))
		{
			library->Unload();
			if (error) *error = "module registration failed for " + path.string();
			return Status::RegistrationFailed;
		}

		m_Entries.push_back({ path.string(), std::move(library), module });
		WLD_CORE_INFO("Module '{0}' loaded and registered", module->Id ? module->Id : "(unnamed)");
		return Status::Ok;
	}

	void ModuleManager::UnloadAll(WorldContext& context)
	{
		for (auto it = m_Entries.rbegin(); it != m_Entries.rend(); ++it)
		{
			if (it->Module && it->Module->Unregister)
				it->Module->Unregister(context);
			if (it->Library)
				it->Library->Unload();
		}
		m_Entries.clear();
	}
}
