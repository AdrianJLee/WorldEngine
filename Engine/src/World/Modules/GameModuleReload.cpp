#include "wldpch.h"

#include "World/Modules/GameModuleReload.h"
#include "World/Core/WorldContext.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Scene/Scene.h"

#include <filesystem>
#include <system_error>

namespace World::Modules
{
	namespace
	{
		// 回滚副本命名:同目录、同 ABI 只保留最近一份(重载失败回滚用,不入库)。
		std::string RollbackFileName()
		{
			return "Game.rollback-" + std::to_string(WE_MODULE_ABI_VERSION) + ".dll";
		}

		// 卸载时记下"用户真正在改的那份 Game.dll"(卸载后回滚副本可能已成为已加载模块,
		// 但下一次重载的目标始终是规范路径;回滚副本只作为加载失败时的兜底)。
		std::filesystem::path s_LastReloadTarget;
	}

	bool GameModuleReload::IsUnloaded(const WorldContext& context)
	{
		return context.Modules().FindById(GameModuleId) == nullptr;
	}

	bool GameModuleReload::Unload(WorldContext& context, Scene* scene, GameModuleReloadResult* result)
	{
		GameModuleReloadResult local;
		GameModuleReloadResult& out = result ? *result : local;
		out = GameModuleReloadResult {};

		if (scene && !scene->CanApplyScriptReload())
		{
			out.Status = ModuleManager::Status::NotSafePoint;
			out.Message = "not at a script reload safe point (inside a callback, structural commit or stop)";
			return false;
		}

		const std::filesystem::path loaded = context.Modules().PathOf(GameModuleId);
		if (loaded.empty())
		{
			out.Status = ModuleManager::Status::NotFound;
			out.Message = "Game module is not loaded";
			return false;
		}
		std::filesystem::path target = GameModuleHost::ResolveDefaultPath();
		if (target.empty() || !std::filesystem::exists(target))
			target = loaded;   // 打包布局/自定义路径:以当前已加载文件为准
		s_LastReloadTarget = target;
		out.ModulePath = target.string();

		// 1) 回滚副本(卸载前拷贝当前 DLL;失败只记诊断,不阻断重载)。
		const std::filesystem::path rollback = loaded.parent_path() / RollbackFileName();
		std::error_code copyError;
		std::filesystem::copy_file(loaded, rollback, std::filesystem::copy_options::overwrite_existing, copyError);
		if (copyError)
			out.Diagnostics.push_back("rollback copy failed: " + copyError.message());
		else
			out.RollbackPath = rollback.string();

		// 2) 卸载模块(schema + FreeLibrary)。
		std::string error;
		out.Status = context.Modules().Unload(GameModuleId, context, &error);
		if (out.Status != ModuleManager::Status::Ok)
		{
			out.Message = "unload failed: " + error;
			return false;
		}
		out.ModuleUnloaded = true;
		out.Message = "Game module unloaded, file can be rebuilt";
		return true;
	}

	bool GameModuleReload::Load(WorldContext& context, Scene* scene, GameModuleReloadResult* result)
	{
		GameModuleReloadResult local;
		GameModuleReloadResult& out = result ? *result : local;
		out = GameModuleReloadResult {};

		if (context.Modules().FindById(GameModuleId))
		{
			out.Status = ModuleManager::Status::AlreadyLoaded;
			out.Message = "Game module is already loaded";
			return false;
		}
		if (scene && !scene->CanApplyScriptReload())
		{
			out.Status = ModuleManager::Status::NotSafePoint;
			out.Message = "not at a script reload safe point (inside a callback, structural commit or stop)";
			return false;
		}

		std::filesystem::path path = s_LastReloadTarget;
		if (path.empty() || !std::filesystem::exists(path))
			path = GameModuleHost::ResolveDefaultPath();
		out.ModulePath = path.string();

		std::string error;
		out.Status = context.Modules().Load(path, context, &error);
		if (out.Status == ModuleManager::Status::Ok)
		{
			if (const WeModule* module = context.Modules().FindById(GameModuleId))
				out.AbiVersion = module->AbiVersion;
			out.Message = "Game module loaded";
			return true;
		}

		out.Message = "load failed: " + error;
		const std::filesystem::path rollback = path.parent_path() / RollbackFileName();
		if (!std::filesystem::exists(rollback))
		{
			out.Diagnostics.push_back("no rollback copy at " + rollback.string());
			return false;
		}
		std::string rollbackError;
		const ModuleManager::Status rollbackStatus = context.Modules().Load(rollback, context, &rollbackError);
		if (rollbackStatus != ModuleManager::Status::Ok)
		{
			out.Diagnostics.push_back("rollback load failed: " + rollbackError);
			return false;
		}
		out.RolledBack = true;
		out.RollbackPath = rollback.string();
		if (const WeModule* module = context.Modules().FindById(GameModuleId))
			out.AbiVersion = module->AbiVersion;
		out.Message += "; rolled back to the previous module copy";
		return false;   // 重载本身失败(旧行为已恢复,由编辑器显示诊断)
	}

	bool GameModuleReload::Reload(WorldContext& context, Scene* scene, GameModuleReloadResult* result)
	{
		GameModuleReloadResult unloaded;
		if (!Unload(context, scene, &unloaded))
		{
			if (result) *result = unloaded;
			return false;
		}

		GameModuleReloadResult loaded;
		const bool ok = Load(context, scene, &loaded);
		if (result)
		{
			*result = loaded;
			result->InstancesDrained = unloaded.InstancesDrained;
			result->ModuleUnloaded = false;   // 一步式结束后模块窗口已关闭(ok)或回滚成功
			if (result->ModulePath.empty()) result->ModulePath = unloaded.ModulePath;
			if (result->RollbackPath.empty()) result->RollbackPath = unloaded.RollbackPath;
			result->Diagnostics.insert(result->Diagnostics.begin(),
				unloaded.Diagnostics.begin(), unloaded.Diagnostics.end());
			if (!result->Message.empty())
				result->Message = unloaded.Message + " -> " + result->Message;
		}
		return ok;
	}
}
