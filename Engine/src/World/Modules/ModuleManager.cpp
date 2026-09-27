#include "wldpch.h"
#include "World/Modules/ModuleManager.h"
#include "World/Core/WorldContext.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/BehaviorRegistry.h"
#include "World/Utils/DynamicLibrary.h"

#include <algorithm>
#include <vector>

namespace World::Modules
{
	namespace
	{
		// ABI/入口等值门(唯一实现):Load 与热重载候选校验共用,不复制第二份。
		const char* ModuleContractError(const WeModule* module)
		{
			if (!module) return "module query returned null (host ABI rejected)";
			if (module->StructSize != sizeof(WeModule)) return "module struct size mismatch";
			if (module->AbiVersion != WE_MODULE_ABI_VERSION) return "module ABI version mismatch";
			if (!module->Register) return "module has no registration entry";
			return nullptr;
		}

		// 当前注册表里的脚本行为 id 清单(卸载前后取差集 = 该模块的脚本类型)。
		std::vector<std::string> ScriptBehaviorIds(const Schema::SchemaRegistry& schemas)
		{
			std::vector<std::string> ids;
			for (const Schema::TypeSchema* type : schemas.List(Schema::TypeCategory::Script))
				if (type)
					ids.push_back(BehaviorRegistry::NativeModuleId(*type));
			return ids;
		}
	}

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
			case Status::AlreadyLoaded: return "module id already loaded";
			case Status::NotSafePoint: return "not at a script reload safe point";
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
		if (const char* contractError = ModuleContractError(module))
		{
			library->Unload();
			if (error) *error = std::string(contractError) + " (" + path.string() + ")";
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
		// CPPT-2(F-5):Category==Script 即行为清单的事实源 —— 模块注册成功后刷新 BehaviorRegistry,
		// 去掉"schema 一本账、BehaviorRegistry 无人用"的双轨。
		ScriptEngine::EnsureSchemaBehaviors(context.Schemas(), nullptr);
		return Status::Ok;
	}

	ModuleManager::Status ModuleManager::UnloadAt(size_t index, WorldContext& context, std::string* error)
	{
		if (index >= m_Entries.size())
		{
			if (error) *error = "module index out of range";
			return Status::NotFound;
		}

		Entry entry = std::move(m_Entries[index]);
		m_Entries.erase(m_Entries.begin() + static_cast<std::ptrdiff_t>(index));

		const std::string moduleId = entry.Module && entry.Module->Id ? entry.Module->Id : std::string("(unnamed)");
		const std::vector<std::string> before = ScriptBehaviorIds(context.Schemas());
		if (entry.Module && entry.Module->Unregister)
			entry.Module->Unregister(context);
		const std::vector<std::string> after = ScriptBehaviorIds(context.Schemas());
		// 差集 = 这次卸载真的拿掉的脚本类型:把行为描述一并摘掉,避免重载后残留旧消息。
		for (const std::string& id : before)
			if (std::find(after.begin(), after.end(), id) == after.end())
				BehaviorRegistry::Instance().Unregister(id);
		if (entry.Library)
			entry.Library->Unload();
		WLD_CORE_INFO("Module '{0}' unloaded", moduleId);
		return Status::Ok;
	}

	ModuleManager::Status ModuleManager::Unload(const std::string& moduleId, WorldContext& context, std::string* error)
	{
		for (size_t index = 0; index < m_Entries.size(); ++index)
		{
			const WeModule* module = m_Entries[index].Module;
			if (module && module->Id && moduleId == module->Id)
				return UnloadAt(index, context, error);
		}
		if (error) *error = "module not loaded: " + moduleId;
		return Status::NotFound;
	}

	const WeModule* ModuleManager::FindById(const std::string& moduleId) const
	{
		for (const Entry& entry : m_Entries)
			if (entry.Module && entry.Module->Id && moduleId == entry.Module->Id)
				return entry.Module;
		return nullptr;
	}

	std::filesystem::path ModuleManager::PathOf(const std::string& moduleId) const
	{
		for (const Entry& entry : m_Entries)
			if (entry.Module && entry.Module->Id && moduleId == entry.Module->Id)
				return entry.Path;
		return {};
	}

	void ModuleManager::UnloadAll(WorldContext& context)
	{
		for (auto it = m_Entries.rbegin(); it != m_Entries.rend(); ++it)
		{
			const std::string moduleId = it->Module && it->Module->Id ? it->Module->Id : std::string();
			const std::vector<std::string> before = ScriptBehaviorIds(context.Schemas());
			if (it->Module && it->Module->Unregister)
				it->Module->Unregister(context);
			const std::vector<std::string> after = ScriptBehaviorIds(context.Schemas());
			for (const std::string& id : before)
				if (std::find(after.begin(), after.end(), id) == after.end())
					BehaviorRegistry::Instance().Unregister(id);
			if (it->Library)
				it->Library->Unload();
		}
		m_Entries.clear();
	}
}
