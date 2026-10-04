#include "wldpch.h"

#include "World/Plugins/PluginManager.h"

#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Schema/SchemaRegistry.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <system_error>
#include <utility>

namespace World::Plugins
{
	namespace
	{
		// 回滚副本命名:与 Modules/GameModuleReload 同一思路(同目录、按插件 ABI 只留最近一份)。
		std::filesystem::path RollbackPathFor(const std::filesystem::path& libraryPath)
		{
			const std::string name = libraryPath.stem().string() + ".rollback-"
				+ std::to_string(WE_PLUGIN_ABI_VERSION) + libraryPath.extension().string();
			return libraryPath.parent_path() / name;
		}

		std::string Hex32(uint32_t value)
		{
			char buffer[16] = {};
			std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
			return buffer;
		}

		// 诊断上限:逐条诊断会按"实体×字段"放大,超限后只留计数(见 result.InstancesSkipped)。
		constexpr std::size_t kMaxReloadDiagnostics = 16;

		void PushDiagnostic(PluginReloadResult& result, const std::string& text)
		{
			if (result.Diagnostics.size() < kMaxReloadDiagnostics)
				result.Diagnostics.push_back(text);
		}

		// 目标场景里某个组件 id 的实例数(不触发结构写;场景可能处于 Play)。
		std::size_t CountInstancesInScene(const Scene& scene, uint32_t componentId)
		{
			const entt::registry& registry = scene.GetRegistry();
			const auto* storage = registry.storage(componentId);
			return storage ? storage->size() : 0;
		}

		// 快照的字段匹配:优先字段 id(名字的 FNV-1a),回退字段名;都没有 = nullptr。
		const World::Schema::FieldSchema* FindRestoreField(const World::Schema::TypeSchema& schema,
			uint64_t fieldId, const std::string& name)
		{
			const World::Schema::FieldSchema* byName = nullptr;
			for (const World::Schema::FieldSchema& field : schema.Fields)
			{
				if (field.Id.Value == fieldId)
					return &field;
				if (byName == nullptr && field.Name == name)
					byName = &field;
			}
			return byName;
		}
	}

	// ---- T6:泄漏审计面 ---------------------------------------------------------------

	PluginLedgerCounts PluginManager::LedgerCounts(const std::string& id) const
	{
		PluginLedgerCounts counts;
		const Record* record = FindRecord(id);
		if (!record)
			return counts;
		counts.AssetTypes = record->RegisteredAssetTypes.size();
		counts.Importers = record->RegisteredImporters.size();
		counts.Components = record->RegisteredComponents.size();
		counts.EditorCommands = record->RegisteredEditorCommands.size();
		counts.EditorPanels = record->RegisteredEditorPanels.size();
		counts.ScriptFunctions = record->RegisteredScriptFunctions.size();
		return counts;
	}

	PluginLedgerCounts PluginManager::LedgerTotals() const
	{
		PluginLedgerCounts counts;
		for (const Record& record : m_Records)
		{
			counts.AssetTypes += record.RegisteredAssetTypes.size();
			counts.Importers += record.RegisteredImporters.size();
			counts.Components += record.RegisteredComponents.size();
			counts.EditorCommands += record.RegisteredEditorCommands.size();
			counts.EditorPanels += record.RegisteredEditorPanels.size();
			counts.ScriptFunctions += record.RegisteredScriptFunctions.size();
		}
		return counts;
	}

	std::size_t PluginManager::LiveComponentInstances(const std::string& id,
		const WorldContext* context) const
	{
		const Record* record = FindRecord(id);
		if (!record)
			return 0;
		std::size_t total = 0;
		for (const RegisteredComponent& item : record->RegisteredComponents)
			if (item.Schema.Storage)
				total += World::Scene::CountLiveComponentInstances(item.Schema.Storage->ComponentId, context);
		return total;
	}

	bool PluginManager::HasPendingReload(const std::string& id) const
	{
		return m_PendingReloads.find(id) != m_PendingReloads.end();
	}

	std::string PluginManager::LoadedLibraryPath(const std::string& id) const
	{
		const Record* record = FindRecord(id);
		return record && record->Entry.State == PluginState::Loaded
			? record->LoadedLibraryPath.string() : std::string();
	}

	// ---- PLUG-CLEAN-1:面板"重新加载"请求(帧边界执行;契约见 PluginManager.h) --------

	void PluginManager::RequestReload(const std::string& id)
	{
		if (id.empty())
			return;
		// 去重:同一帧/连续点击只登记一次(执行顺序 = 登记顺序,确定性)。
		if (std::find(m_ReloadRequests.begin(), m_ReloadRequests.end(), id) == m_ReloadRequests.end())
			m_ReloadRequests.push_back(id);
	}

	std::vector<std::string> PluginManager::ConsumeReloadRequests()
	{
		std::vector<std::string> requests = std::move(m_ReloadRequests);
		m_ReloadRequests.clear();
		return requests;
	}

	void PluginManager::DiscardPendingReload(const std::string& id)
	{
		m_PendingReloads.erase(id);
	}

	// ---- T6:第二段的清单重读 / 指定路径加载 ------------------------------------------

	bool PluginManager::RefreshManifest(Record& record, std::string* error)
	{
		const PluginManifest& current = record.Entry.Manifest;
		const std::filesystem::path manifestPath = current.ManifestPath;
		std::error_code ec;
		if (manifestPath.empty() || !std::filesystem::is_regular_file(manifestPath, ec))
		{
			if (error)
				*error = "plugin manifest not found: " + manifestPath.string();
			return false;
		}

		// 事务化:解析成功且 id 不变才替换条目清单(失败时条目保持原样,回滚副本仍可用)。
		PluginManifest parsed;
		std::string parseError;
		if (!PluginManifest::Load(manifestPath, current.Scope, &parsed, &parseError))
		{
			if (error)
				*error = parseError.empty() ? ("plugin manifest rejected: " + manifestPath.string())
					: parseError;
			return false;
		}
		if (parsed.Id != current.Id)
		{
			if (error)
				*error = "plugin id changed in manifest ('" + current.Id + "' -> '" + parsed.Id
					+ "'); reload refused (unload and rediscover instead)";
			return false;
		}

		// 产物定位与 Discover 同一规则:包自带 bin/ 优先,其次调用方给的开发构建根。
		if (!std::filesystem::is_regular_file(parsed.LibraryPath, ec))
		{
			for (const std::filesystem::path& devRoot : m_DevBinaryRoots)
			{
				const std::filesystem::path candidate =
					devRoot / (parsed.Root.filename().string() + kPluginLibraryExtension);
				if (std::filesystem::is_regular_file(candidate, ec))
				{
					parsed.LibraryPath = candidate;
					break;
				}
			}
		}

		record.Entry.Manifest = std::move(parsed);
		// 被拒绝的条目(坏 DLL / 坏清单)重新拿到清单后允许再试一次(Reload 是显式重试入口)。
		if (record.Entry.State == PluginState::Rejected)
		{
			record.Entry.State = PluginState::Discovered;
			record.Entry.Diagnostic.clear();
		}
		return true;
	}

	PluginManager::Status PluginManager::LoadFrom(const std::string& id,
		const std::filesystem::path& libraryPath, WorldContext& context, std::string* error)
	{
		Record* record = FindRecord(id);
		if (!record)
		{
			if (error) *error = "plugin not found: " + id;
			return Status::NotFound;
		}
		// 临时把目标路径换成调用方给的产物(回滚副本),加载完成后恢复"重编目标" ——
		// Manifest.LibraryPath 语义保持 = 下一次 reload 尝试的那份文件。
		const std::filesystem::path target = record->Entry.Manifest.LibraryPath;
		record->Entry.Manifest.LibraryPath = libraryPath;
		const Status status = Load(id, context, error);
		record->Entry.Manifest.LibraryPath = target;
		return status;
	}

	// ---- T6:第一段(快照 + 移除 + 卸载) ------------------------------------------------

	PluginManager::Status PluginManager::SnapshotAndDrainInstances(Record& record,
		WorldContext& context, Scene* scene, PendingReload& pending, PluginReloadResult& result)
	{
		const std::string id = record.Entry.Manifest.Id;

		// 目标场景之外的活实例:不偷偷改写别的场景(与 Unload 的 T2c 门同口径)。
		if (scene)
		{
			std::size_t outside = 0;
			std::string report;
			for (const RegisteredComponent& item : record.RegisteredComponents)
			{
				if (!item.Schema.Storage)
					continue;
				const std::size_t total = World::Scene::CountLiveComponentInstances(
					item.Schema.Storage->ComponentId, &context);
				const std::size_t inside = CountInstancesInScene(*scene, item.Schema.Storage->ComponentId);
				if (total > inside)
				{
					outside += total - inside;
					if (!report.empty()) report += ", ";
					report += item.Id + " x" + std::to_string(total - inside);
				}
			}
			if (outside > 0)
			{
				result.Message = "plugin '" + id + "' has " + std::to_string(outside)
					+ " live component instance(s) outside the target scene (" + report
					+ "); destroy those scenes or stop Play before reloading";
				Log(WePluginLogWarn, result.Message);
				return Status::HasLiveInstances;
			}
		}

		for (const RegisteredComponent& item : record.RegisteredComponents)
		{
			if (!item.Schema.Storage || !scene)
				continue;

			const uint32_t componentId = item.Schema.Storage->ComponentId;
			// 只读快照(走 const 注册表;Play 下也不触发结构写断言)。
			const entt::registry& registry = static_cast<const Scene&>(*scene).GetRegistry();
			const auto* storage = registry.storage(componentId);
			if (!storage || storage->size() == 0)
				continue;

			std::vector<entt::entity> handles;
			handles.reserve(storage->size());
			for (const entt::entity handle : *storage)
				handles.push_back(handle);

			for (const entt::entity handle : handles)
			{
				PluginComponentInstanceSnapshot shot;
				shot.ComponentId = componentId;
				shot.ComponentType = item.Id;
				if (const auto* identity = registry.try_get<World::UUIDComponent>(handle))
					shot.EntityId = identity->ID;
				else
					PushDiagnostic(result, "entity " + std::to_string(static_cast<uint32_t>(handle))
						+ " has no UUIDComponent; its " + item.Id + " value cannot be restored");
				const void* blob = storage->value(handle);
				for (const World::Schema::FieldSchema& field : item.Schema.Fields)
				{
					if (!field.Get)
						continue;
					PluginComponentFieldSnapshot fieldShot;
					fieldShot.FieldId = field.Id.Value;
					fieldShot.Name = field.Name;
					fieldShot.Value = field.Get(blob);
					shot.Fields.push_back(std::move(fieldShot));
				}
				pending.Instances.push_back(std::move(shot));
			}

			// 结构写(走 Scene::GetRegistry 的 owner 线程 + 结构写门禁;Play = 抛异常 ⇒ 干净拒绝)。
			entt::registry& writable = scene->GetRegistry();
			for (const entt::entity handle : handles)
				RemovePluginComponentInstance(writable, componentId, handle);
		}

		pending.InstancesSnapshotted = static_cast<std::uint32_t>(pending.Instances.size());
		result.InstancesSnapshotted = pending.InstancesSnapshotted;
		return Status::Ok;
	}

	PluginManager::Status PluginManager::UnloadForReload(const std::string& id,
		WorldContext& context, Scene* scene, PluginReloadResult* result)
	{
		PluginReloadResult local;
		PluginReloadResult& out = result ? *result : local;
		out = PluginReloadResult {};
		out.PluginId = id;

		Record* record = FindRecord(id);
		if (!record)
		{
			out.Message = "plugin not found: " + id;
			return Status::NotFound;
		}
		if (record->Entry.State != PluginState::Loaded)
		{
			out.Message = "plugin is not loaded: " + id;
			return Status::NotLoaded;
		}
		if (scene && !scene->CanApplyModuleReload())
		{
			out.Message = "not at a script reload safe point (inside a callback, structural commit or stop); try again at the frame boundary";
			return Status::NotSafePoint;
		}
		// 依赖门:在动场景之前先查,避免"先移除实例再发现不能卸载"。
		{
			std::string dependents;
			for (const Record& other : m_Records)
			{
				if (&other == record || other.Entry.State != PluginState::Loaded)
					continue;
				const std::vector<std::string>& depends = other.Entry.Manifest.Depends;
				if (std::find(depends.begin(), depends.end(), id) == depends.end())
					continue;
				if (!dependents.empty()) dependents += ", ";
				dependents += other.Entry.Manifest.Id;
			}
			if (!dependents.empty())
			{
				out.Message = "plugin '" + id + "' is still required by loaded plugin(s): " + dependents;
				return Status::HasLoadedDependents;
			}
		}

		const std::filesystem::path loadedPath = record->LoadedLibraryPath.empty()
			? record->Entry.Manifest.LibraryPath : record->LoadedLibraryPath;
		const std::filesystem::path targetPath = record->Entry.Manifest.LibraryPath.empty()
			? loadedPath : record->Entry.Manifest.LibraryPath;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(loadedPath, ec))
		{
			out.Message = "plugin library not found: " + loadedPath.string();
			return Status::LoadFailed;
		}
		out.LibraryPath = targetPath.string();

		// 1) 卸载前的回滚副本(当前加载的那份;失败只记诊断,不阻断重载)。
		const std::filesystem::path rollbackPath = RollbackPathFor(loadedPath);
		if (rollbackPath != loadedPath)
		{
			std::error_code copyError;
			std::filesystem::copy_file(loadedPath, rollbackPath,
				std::filesystem::copy_options::overwrite_existing, copyError);
			if (copyError)
				PushDiagnostic(out, "rollback copy failed: " + copyError.message());
			else
				out.RollbackPath = rollbackPath.string();
		}
		else
		{
			// 当前加载的就是上一次的回滚副本(损坏构建后回滚过):原样保留。
			out.RollbackPath = rollbackPath.string();
		}

		// 2) 快照 + 从目标场景移除(见函数头;其它场景的实例门在上一步已经查过)。
		PendingReload pending;
		pending.TargetPath = targetPath;
		pending.RollbackPath = rollbackPath;
		const Status snapshot = SnapshotAndDrainInstances(*record, context, scene, pending, out);
		if (snapshot != Status::Ok)
			return snapshot;

		// 3) 卸载(释放 DLL 文件锁)。失败 = 把刚移除的实例写回(旧 schema 仍在)。
		std::string error;
		const Status status = UnloadRecord(*record, context, &error, true);
		if (status != Status::Ok)
		{
			PluginReloadResult restore;
			RestoreInstances(*record, context, scene, pending, restore);
			out.Message = "unload for reload failed: " + error;
			out.Diagnostics.insert(out.Diagnostics.end(), restore.Diagnostics.begin(),
				restore.Diagnostics.end());
			out.InstancesRestored = restore.InstancesRestored;
			out.InstancesSkipped = restore.InstancesSkipped;
			return status;
		}

		if (m_PendingReloads.find(id) != m_PendingReloads.end())
			Log(WePluginLogWarn, "reload overwrote an unfinished pending snapshot id=" + id);
		m_PendingReloads[id] = std::move(pending);

		out.Unloaded = true;
		out.Ok = true;
		out.ResultPhase = PluginReloadResult::Phase::Unloaded;
		out.Message = "plugin '" + id + "' unloaded (" + std::to_string(out.InstancesSnapshotted)
			+ " component instance(s) snapshotted); rebuild " + targetPath.string()
			+ " and run reload again";
		Log(WePluginLogInfo, "reload phase 1 id=" + id + " target=" + targetPath.string()
			+ " rollback=" + out.RollbackPath + " instances="
			+ std::to_string(out.InstancesSnapshotted));
		return Status::Ok;
	}

	// ---- T6:第二段(载入新 DLL + 写回快照 / 回滚) --------------------------------------

	void PluginManager::RestoreInstances(const Record& record, WorldContext& context, Scene* scene,
		const PendingReload& pending, PluginReloadResult& result)
	{
		if (pending.Instances.empty())
			return;

		for (const PluginComponentInstanceSnapshot& shot : pending.Instances)
		{
			Scene* foundScene = nullptr;
			entt::entity entity = entt::null;
			if (!World::Scene::FindLiveEntity(shot.EntityId, &context, scene, &foundScene, &entity)
				|| foundScene == nullptr)
			{
				++result.InstancesSkipped;
				PushDiagnostic(result, "entity " + std::to_string(static_cast<uint64_t>(shot.EntityId))
					+ " no longer exists; its " + shot.ComponentType + " instance was not restored");
				continue;
			}
			const RegisteredComponent* target = nullptr;
			for (const RegisteredComponent& item : record.RegisteredComponents)
				if (item.Id == shot.ComponentType)
				{
					target = &item;
					break;
				}
			if (!target || !target->Schema.Storage)
			{
				++result.InstancesSkipped;
				PushDiagnostic(result, "component type '" + shot.ComponentType
					+ "' is no longer registered with storage; instance not restored");
				continue;
			}

			entt::registry& registry = foundScene->GetRegistry();
			const uint32_t componentId = target->Schema.Storage->ComponentId;
			AddPluginComponentInstance(registry, componentId, entity);
			void* blob = PluginComponentInstancePointer(registry, componentId, entity);
			if (!blob)
			{
				++result.InstancesSkipped;
				PushDiagnostic(result, "could not attach component " + shot.ComponentType + " ("
					+ Hex32(componentId) + ") to entity "
					+ std::to_string(static_cast<uint64_t>(shot.EntityId)));
				continue;
			}
			++result.InstancesRestored;
			for (const PluginComponentFieldSnapshot& fieldShot : shot.Fields)
			{
				const World::Schema::FieldSchema* field = FindRestoreField(target->Schema,
					fieldShot.FieldId, fieldShot.Name);
				if (!field || !field->Set)
				{
					++result.InstancesSkipped;
					PushDiagnostic(result, "field '" + fieldShot.Name + "' of " + shot.ComponentType
						+ " no longer exists; value not restored");
					continue;
				}
				// 插件组件只支持定长 POD:Value 的 variant 下标与 Kind 一一对应(WePluginApi 保证),
				// 类型变了 = 不写(避免把 float 塞进 int 之类的静默错值)。
				if (fieldShot.Value.index() != static_cast<std::size_t>(field->K))
				{
					++result.InstancesSkipped;
					PushDiagnostic(result, "field '" + fieldShot.Name + "' of " + shot.ComponentType
						+ " changed kind; value not restored");
					continue;
				}
				field->Set(blob, fieldShot.Value);
			}
		}
	}

	PluginManager::Status PluginManager::LoadForReload(const std::string& id, WorldContext& context,
		Scene* scene, PluginReloadResult* result)
	{
		PluginReloadResult local;
		PluginReloadResult& out = result ? *result : local;
		out = PluginReloadResult {};
		out.PluginId = id;

		Record* record = FindRecord(id);
		if (!record)
		{
			out.Message = "plugin not found: " + id;
			return Status::NotFound;
		}
		if (record->Entry.State == PluginState::Loaded)
		{
			out.Message = "plugin is already loaded: " + id
				+ " (run reload again to unload it first)";
			return Status::AlreadyLoaded;
		}
		if (scene && !scene->CanApplyModuleReload())
		{
			out.Message = "not at a script reload safe point (inside a callback, structural commit or stop); try again at the frame boundary";
			return Status::NotSafePoint;
		}

		const auto pendingIt = m_PendingReloads.find(id);
		const bool hasPending = pendingIt != m_PendingReloads.end();

		// 1) 重读清单(拾取版本/声明变化);失败 = 走回滚分支(旧清单与旧 DLL 仍在)。
		std::string manifestError;
		const bool manifestOk = RefreshManifest(*record, &manifestError);
		if (!manifestOk)
			PushDiagnostic(out, "manifest reload: " + manifestError);

		std::filesystem::path targetPath = hasPending ? pendingIt->second.TargetPath
			: record->Entry.Manifest.LibraryPath;
		if (targetPath.empty())
			targetPath = record->Entry.Manifest.LibraryPath;
		if (!targetPath.empty() && std::filesystem::exists(targetPath))
			record->Entry.Manifest.LibraryPath = targetPath;
		out.LibraryPath = record->Entry.Manifest.LibraryPath.string();

		// 2) 载入新 DLL(清单坏了就当成加载失败,交给回滚分支)。
		std::string error;
		Status status = Status::Rejected;
		if (manifestOk)
			status = Load(id, context, &error);
		else
			error = manifestError;

		if (status == Status::Ok)
		{
			if (hasPending)
			{
				RestoreInstances(*record, context, scene, pendingIt->second, out);
				out.RollbackPath = pendingIt->second.RollbackPath.string();
				DiscardPendingReload(id);
			}
			out.Ok = true;
			out.ResultPhase = PluginReloadResult::Phase::Loaded;
			out.Message = "plugin '" + id + "' loaded ("
				+ std::to_string(out.InstancesRestored) + " component instance(s) restored"
				+ (out.InstancesSkipped ? ", " + std::to_string(out.InstancesSkipped) + " skipped" : "")
				+ ")";
			Log(WePluginLogInfo, "reload phase 2 id=" + id + " path=" + out.LibraryPath
				+ " restored=" + std::to_string(out.InstancesRestored)
				+ " skipped=" + std::to_string(out.InstancesSkipped));
			return Status::Ok;
		}

		out.Message = "load failed: " + error;
		PushDiagnostic(out, "new library load failed: " + error);
		if (!hasPending)
		{
			out.ResultPhase = PluginReloadResult::Phase::Failed;
			return status;
		}

		// 3) 回滚到卸载前拷贝的旧 DLL,并用旧 schema 写回快照。
		const std::filesystem::path rollbackPath = pendingIt->second.RollbackPath;
		std::error_code ec;
		if (rollbackPath.empty() || !std::filesystem::is_regular_file(rollbackPath, ec))
		{
			PushDiagnostic(out, "no rollback copy at " + rollbackPath.string());
			out.ResultPhase = PluginReloadResult::Phase::Failed;
			return status;
		}
		std::string rollbackError;
		const Status rolled = LoadFrom(id, rollbackPath, context, &rollbackError);
		if (rolled != Status::Ok)
		{
			PushDiagnostic(out, "rollback load failed: " + rollbackError);
			out.Message += "; rollback load failed";
			out.ResultPhase = PluginReloadResult::Phase::Failed;
			return status;
		}

		RestoreInstances(*record, context, scene, pendingIt->second, out);
		out.RollbackPath = rollbackPath.string();
		out.RolledBack = true;
		out.ResultPhase = PluginReloadResult::Phase::RolledBack;
		out.Message += "; rolled back to the previous DLL copy ("
			+ std::to_string(out.InstancesRestored) + " instance(s) restored)";
		DiscardPendingReload(id);
		Log(WePluginLogWarn, "reload rolled back id=" + id + " path=" + rollbackPath.string());
		return status;
	}
}
