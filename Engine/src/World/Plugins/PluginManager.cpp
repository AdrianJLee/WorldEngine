#include "wldpch.h"
#include "World/Plugins/PluginManager.h"
#include "World/Core/WorldContext.h"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace World::Plugins
{
	namespace
	{
		std::string JoinIds(const std::set<std::string>& ids)
		{
			std::string text;
			for (const std::string& id : ids)
			{
				if (!text.empty())
					text += ",";
				text += id;
			}
			return text;
		}
	}

	const char* PluginManager::StatusName(Status status)
	{
		switch (status)
		{
			case Status::Ok: return "ok";
			case Status::NotFound: return "plugin not found";
			case Status::LoadFailed: return "plugin library load failed";
			case Status::Rejected: return "plugin entry rejected";
			case Status::AlreadyLoaded: return "plugin already loaded";
			case Status::DependencyNotLoaded: return "plugin dependency is not loaded";
			case Status::HasLoadedDependents: return "plugin still has loaded dependents";
			case Status::NotLoaded: return "plugin is not loaded";
		}
		return "unknown";
	}

	PluginManager::PluginManager()
	{
		m_HostApi.StructSize = sizeof(WeHostApi);
		m_HostApi.AbiVersion = WE_PLUGIN_ABI_VERSION;
		m_HostApi.UserData = nullptr;
		m_HostApi.Log = &PluginManager::LogBridge;
	}

	PluginManager::~PluginManager()
	{
		if (LoadedCount() > 0)
			Log(WePluginLogError, "manager destroyed with " + std::to_string(LoadedCount())
				+ " plugin(s) still loaded (host must call UnloadAll first)");
	}

	void PluginManager::Log(int level, const std::string& text)
	{
		// 统一 `[plugin]` 前缀;用 "{0}" 承载正文,避免正文里的花括号被当成 spdlog 格式串。
		const std::string line = "[plugin] " + text;
		switch (level)
		{
			case WePluginLogWarn: WLD_CORE_WARN("{0}", line); break;
			case WePluginLogError: WLD_CORE_ERROR("{0}", line); break;
			default: WLD_CORE_INFO("{0}", line); break;
		}
	}

	void PluginManager::LogBridge(void* userData, int level, const char* message)
	{
		// UserData = 本次加载的 HostApiBox(插件只原样回传);前缀里带上插件 id。
		const auto* box = static_cast<const HostApiBox*>(userData);
		if (!box)
			return;
		Log(level, box->PluginId + ": " + (message ? message : ""));
	}

	std::vector<PluginEntry> PluginManager::Entries() const
	{
		std::vector<PluginEntry> entries;
		entries.reserve(m_Records.size());
		for (const Record& record : m_Records)
			entries.push_back(record.Entry);
		return entries;
	}

	size_t PluginManager::LoadedCount() const
	{
		size_t count = 0;
		for (const Record& record : m_Records)
			if (record.Entry.State == PluginState::Loaded)
				++count;
		return count;
	}

	std::vector<std::string> PluginManager::LoadOrder() const
	{
		std::vector<const Record*> loaded;
		for (const Record& record : m_Records)
			if (record.Entry.State == PluginState::Loaded)
				loaded.push_back(&record);
		std::sort(loaded.begin(), loaded.end(), [](const Record* left, const Record* right)
		{
			return left->Entry.Order < right->Entry.Order;
		});
		std::vector<std::string> ids;
		ids.reserve(loaded.size());
		for (const Record* record : loaded)
			ids.push_back(record->Entry.Manifest.Id);
		return ids;
	}

	PluginManager::Record* PluginManager::FindRecord(const std::string& id)
	{
		for (Record& record : m_Records)
			if (record.Entry.Manifest.Id == id)
				return &record;
		return nullptr;
	}

	const PluginManager::Record* PluginManager::FindRecord(const std::string& id) const
	{
		for (const Record& record : m_Records)
			if (record.Entry.Manifest.Id == id)
				return &record;
		return nullptr;
	}

	const PluginEntry* PluginManager::Find(const std::string& id) const
	{
		const Record* record = FindRecord(id);
		return record ? &record->Entry : nullptr;
	}

	void PluginManager::Reject(Record& record, std::string reason)
	{
		record.Entry.State = PluginState::Rejected;
		record.Entry.Diagnostic = std::move(reason);
		record.Entry.Order = -1;
		Log(WePluginLogError, "rejected id=" + record.Entry.Manifest.Id + " reason=" + record.Entry.Diagnostic);
	}

	bool PluginManager::Discover(const std::filesystem::path& enginePluginsRoot,
		const std::filesystem::path& projectPluginsRoot)
	{
		if (LoadedCount() > 0)
		{
			Log(WePluginLogError, "discover refused: " + std::to_string(LoadedCount())
				+ " plugin(s) still loaded (call UnloadAll first)");
			return false;
		}

		m_Records.clear();
		m_LoadSequence.clear();
		m_NextOrder = 0;

		ScanRoot(enginePluginsRoot, PluginScope::Engine);
		ScanRoot(projectPluginsRoot, PluginScope::Project);

		// 重复 id:后发现的拒绝 + 诊断(方案 §8);已被拒绝的条目不占用 id。
		std::unordered_map<std::string, size_t> firstById;
		for (size_t index = 0; index < m_Records.size(); ++index)
		{
			Record& record = m_Records[index];
			if (record.Entry.State == PluginState::Rejected)
				continue;
			const std::string& id = record.Entry.Manifest.Id;
			const auto found = firstById.find(id);
			if (found != firstById.end())
			{
				Reject(record, "duplicate plugin id '" + id + "' (already declared by "
					+ m_Records[found->second].Entry.Manifest.ManifestPath.string() + ")");
				continue;
			}
			firstById.emplace(id, index);
			Log(WePluginLogInfo, "discovered id=" + id + " scope="
				+ PluginScopeName(record.Entry.Manifest.Scope) + " path="
				+ record.Entry.Manifest.Root.string());
		}

		ValidateDependencies();
		return true;
	}

	void PluginManager::ScanRoot(const std::filesystem::path& root, PluginScope scope)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec))
			return;   // 缺根 = 0 个插件(不是错误,方案 §4.2)

		// 目录名排序 = 确定性发现顺序(重复 id 的胜负、拓扑平局都依赖它)。
		std::vector<std::filesystem::path> directories;
		for (const std::filesystem::directory_entry& item : std::filesystem::directory_iterator(root, ec))
		{
			if (item.is_directory(ec) && !ec)
				directories.push_back(item.path());
			ec.clear();
		}
		std::sort(directories.begin(), directories.end());

		for (const std::filesystem::path& directory : directories)
		{
			// 形态固定(方案 §4.1):<root>/<name>/plugin.we.yaml;没有清单的目录不是插件包。
			const std::filesystem::path manifestPath = directory / "plugin.we.yaml";
			if (!std::filesystem::is_regular_file(manifestPath, ec))
				continue;

			Record record;
			std::string reason;
			if (!PluginManifest::Load(manifestPath, scope, &record.Entry.Manifest, &reason))
			{
				if (record.Entry.Manifest.Id.empty())
					record.Entry.Manifest.Id = directory.filename().string();
				Reject(record, reason);
			}
			m_Records.push_back(std::move(record));
		}
	}

	void PluginManager::ValidateDependencies()
	{
		// 缺依赖 + 被拒绝依赖的级联拒绝(不静默、不半可用);迭代到不动点。
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (Record& record : m_Records)
			{
				if (record.Entry.State != PluginState::Discovered)
					continue;
				for (const std::string& dependency : record.Entry.Manifest.Depends)
				{
					const Record* provider = FindRecord(dependency);
					if (!provider)
					{
						Reject(record, "missing dependency '" + dependency + "'");
						changed = true;
						break;
					}
					if (provider->Entry.State == PluginState::Rejected)
					{
						Reject(record, "dependency '" + dependency + "' could not be loaded");
						changed = true;
						break;
					}
				}
			}
		}

		// 拓扑排序(Kahn;平局按发现顺序取最小索引 = 确定性)。
		m_LoadSequence.clear();
		const size_t count = m_Records.size();
		std::vector<int> inDegree(count, 0);
		for (size_t index = 0; index < count; ++index)
		{
			if (m_Records[index].Entry.State != PluginState::Discovered)
				continue;
			for (const std::string& dependency : m_Records[index].Entry.Manifest.Depends)
				if (FindRecord(dependency))
					++inDegree[index];
		}

		std::vector<bool> consumed(count, false);
		for (;;)
		{
			size_t next = count;
			for (size_t index = 0; index < count; ++index)
			{
				if (!consumed[index] && m_Records[index].Entry.State == PluginState::Discovered
					&& inDegree[index] == 0)
				{
					next = index;
					break;
				}
			}
			if (next == count)
				break;
			consumed[next] = true;
			m_LoadSequence.push_back(next);
			const std::string& loadedId = m_Records[next].Entry.Manifest.Id;
			for (size_t index = 0; index < count; ++index)
			{
				if (consumed[index] || m_Records[index].Entry.State != PluginState::Discovered)
					continue;
				const std::vector<std::string>& depends = m_Records[index].Entry.Manifest.Depends;
				if (std::find(depends.begin(), depends.end(), loadedId) != depends.end())
					--inDegree[index];
			}
		}

		// 排不出的节点 = 环内或依赖环 → 全部干净拒绝(带可读链路)。
		std::vector<size_t> leftovers;
		for (size_t index = 0; index < count; ++index)
			if (!consumed[index] && m_Records[index].Entry.State == PluginState::Discovered)
				leftovers.push_back(index);
		if (leftovers.empty())
			return;

		const std::string chain = CycleChain(m_Records, leftovers);
		for (size_t index : leftovers)
			Reject(m_Records[index], "dependency cycle: " + chain);
	}

	std::string PluginManager::CycleChain(const std::vector<Record>& records, const std::vector<size_t>& nodes)
	{
		std::unordered_map<std::string, size_t> indexById;
		for (size_t index : nodes)
			indexById.emplace(records[index].Entry.Manifest.Id, index);

		std::vector<size_t> path;
		std::unordered_set<size_t> onPath;
		std::unordered_set<size_t> done;
		std::vector<size_t> cycle;

		std::function<bool(size_t)> visit = [&](size_t index) -> bool
		{
			path.push_back(index);
			onPath.insert(index);
			for (const std::string& dependency : records[index].Entry.Manifest.Depends)
			{
				const auto found = indexById.find(dependency);
				if (found == indexById.end())
					continue;
				const size_t next = found->second;
				if (onPath.count(next))
				{
					const auto begin = std::find(path.begin(), path.end(), next);
					cycle.assign(begin, path.end());
					cycle.push_back(next);
					return true;
				}
				if (!done.count(next) && visit(next))
					return true;
			}
			path.pop_back();
			onPath.erase(index);
			done.insert(index);
			return false;
		};

		for (size_t index : nodes)
			if (!done.count(index) && visit(index))
				break;

		std::string chain;
		const std::vector<size_t>& ordered = cycle.empty() ? nodes : cycle;
		for (size_t index : ordered)
		{
			if (!chain.empty())
				chain += " -> ";
			chain += records[index].Entry.Manifest.Id;
		}
		return chain;
	}

	PluginManager::Status PluginManager::LoadRecord(size_t index, WorldContext& context, std::string* error)
	{
		Record& record = m_Records[index];
		PluginEntry& entry = record.Entry;

		if (entry.State == PluginState::Loaded)
		{
			if (error) *error = "plugin already loaded: " + entry.Manifest.Id;
			return Status::AlreadyLoaded;
		}
		if (entry.State == PluginState::Rejected)
		{
			if (error) *error = entry.Diagnostic.empty() ? "plugin entry rejected" : entry.Diagnostic;
			return Status::Rejected;
		}

		// 依赖必须先加载成功(缺依赖/依赖环在发现期已拒绝;这里覆盖"单插件加载顺序不对")。
		for (const std::string& dependency : entry.Manifest.Depends)
		{
			const Record* provider = FindRecord(dependency);
			if (!provider || provider->Entry.State != PluginState::Loaded)
			{
				if (error) *error = !provider
					? "missing dependency '" + dependency + "'"
					: "dependency '" + dependency + "' is not loaded";
				return Status::DependencyNotLoaded;
			}
		}

		// 加载前快照:本函数只在**全部校验 + Register 成功**后才写 record;任何失败路径都让
		// 局部 library 句柄析构释放 DLL,record 保持调用前状态(零半注册)。
		const std::filesystem::path libraryPath = entry.Manifest.LibraryPath;
		if (!std::filesystem::is_regular_file(libraryPath))
		{
			if (error) *error = "plugin library not found: " + libraryPath.string();
			return Status::LoadFailed;
		}

		auto library = std::make_unique<World::DynamicLibrary>();
		if (!library->Load(libraryPath.string()))
		{
			if (error) *error = "plugin library load failed: " + library->GetLastError()
				+ " (" + libraryPath.string() + ")";
			return Status::LoadFailed;
		}

		// 等值门(风格同 ModuleManager::ModuleContractError,但版本号独立):
		// 入口符号 → WePluginQuery(hostAbi) → StructSize → AbiVersion → Register → id。
		const auto query = reinterpret_cast<WePluginQueryFn>(library->GetSymbol(entry.Manifest.Entry.c_str()));
		if (!query)
		{
			if (error) *error = "missing entry symbol '" + entry.Manifest.Entry + "' in " + libraryPath.string();
			return Status::Rejected;
		}
		const WePlugin* plugin = query(WE_PLUGIN_ABI_VERSION);
		if (!plugin)
		{
			if (error) *error = "plugin rejected host plugin ABI " + std::to_string(WE_PLUGIN_ABI_VERSION)
				+ " (WePluginQuery returned null)";
			return Status::Rejected;
		}
		// StructSize 口径(与 WePluginApi.h 的"字段只增不改号"一致,2026-09-30 主 agent 裁决):
		//   * 宿主只读自己已知的 v1 前缀 ⇒ **覆盖该前缀即可接受**;
		//     插件用更新的头编译(尾部追加字段、abi 不变)时 StructSize 更大 = 合法;
		//   * 小于最小前缀 = 老到无法安全读取 ⇒ 干净拒绝(不按新布局解释)。
		// 注意:将来宿主结构扩大后,新增字段的读取必须用
		// `entry.PluginStructSize >= 该字段结束偏移` 逐项把关,而不是收紧这里的门槛。
		constexpr uint32_t kMinPluginStructSize = sizeof(WePlugin);   // T1:宿主读全部 v1 字段
		if (plugin->StructSize < kMinPluginStructSize)
		{
			if (error) *error = "plugin struct too small (plugin=" + std::to_string(plugin->StructSize)
				+ " host requires=" + std::to_string(kMinPluginStructSize) + ")";
			return Status::Rejected;
		}
		entry.PluginStructSize = plugin->StructSize;
		entry.PluginAbi = plugin->AbiVersion;
		if (plugin->AbiVersion != WE_PLUGIN_ABI_VERSION)
		{
			if (error) *error = "plugin ABI version mismatch (plugin=" + std::to_string(plugin->AbiVersion)
				+ " host=" + std::to_string(WE_PLUGIN_ABI_VERSION) + ")";
			return Status::Rejected;
		}
		if (!plugin->Register)
		{
			if (error) *error = "plugin has no Register entry";
			return Status::Rejected;
		}
		const std::string pluginId = plugin->Id ? plugin->Id : "";
		if (pluginId.empty() || pluginId != entry.Manifest.Id)
		{
			if (error) *error = "plugin id '" + pluginId + "' does not match manifest id '"
				+ entry.Manifest.Id + "'";
			return Status::Rejected;
		}

		// 声明比对是**警告**不是拒绝(WePluginApi.h:缺声明 = 警告)。
		std::set<std::string> declared;
		if (plugin->ProvidesCount > 0 && !plugin->Provides)
			Log(WePluginLogWarn, "warning id=" + pluginId + " provides count > 0 but the table is null");
		for (uint32_t i = 0; i < plugin->ProvidesCount && plugin->Provides; ++i)
			if (plugin->Provides[i] && plugin->Provides[i][0])
				declared.insert(plugin->Provides[i]);
		const std::set<std::string> manifestSet(entry.Manifest.Provides.begin(), entry.Manifest.Provides.end());
		if (declared != manifestSet)
			Log(WePluginLogWarn, "warning id=" + pluginId + " provides mismatch struct=["
				+ JoinIds(declared) + "] manifest=[" + JoinIds(manifestSet) + "]");
		const std::string pluginMinimum = plugin->MinEngineVersion ? plugin->MinEngineVersion : "";
		if (!pluginMinimum.empty() && pluginMinimum != entry.Manifest.Engine)
			Log(WePluginLogWarn, "warning id=" + pluginId + " engine mismatch struct='" + pluginMinimum
				+ "' manifest='" + entry.Manifest.Engine + "'");

		auto host = std::make_unique<HostApiBox>();
		host->PluginId = pluginId;
		host->Api = m_HostApi;
		host->Api.UserData = host.get();

		bool registered = false;
		try
		{
			registered = plugin->Register(context, host->Api);
		}
		catch (const std::exception& exception)
		{
			// 契约违约(回调不该抛):best-effort Unregister 回滚可能留下的半注册状态。
			if (plugin->Unregister)
			{
				try { plugin->Unregister(context); }
				catch (...) {}
			}
			if (error) *error = "plugin registration raised an exception: " + std::string(exception.what());
			return Status::Rejected;
		}
		catch (...)
		{
			if (plugin->Unregister)
			{
				try { plugin->Unregister(context); }
				catch (...) {}
			}
			if (error) *error = "plugin registration raised an unknown exception";
			return Status::Rejected;
		}

		if (!registered)
		{
			// 契约:Register 返回 false = 插件自己已清干净,宿主不调用 Unregister(防二次释放)。
			if (error) *error = "plugin registration failed (Register returned false)";
			return Status::Rejected;
		}

		record.Library = std::move(library);
		record.Host = std::move(host);
		record.Plugin = plugin;
		entry.State = PluginState::Loaded;
		entry.Order = m_NextOrder++;
		entry.Diagnostic.clear();
		Log(WePluginLogInfo, "loaded id=" + entry.Manifest.Id + " scope="
			+ PluginScopeName(entry.Manifest.Scope) + " order=" + std::to_string(entry.Order));
		return Status::Ok;
	}

	PluginManager::Status PluginManager::LoadAll(WorldContext& context, std::string* error)
	{
		for (size_t index : m_LoadSequence)
		{
			Record& record = m_Records[index];
			if (record.Entry.State == PluginState::Loaded)
				continue;
			std::string reason;
			const Status status = LoadRecord(index, context, &reason);
			if (status == Status::Ok)
				continue;
			if (reason.empty())
				reason = StatusName(status);
			Reject(record, reason);
		}

		// 汇总:发现期拒绝(清单/重复 id/缺依赖/环)与加载期拒绝都算"本批有失败",
		// 逐条诊断在条目里(Find()/Entries())。
		size_t rejected = 0;
		std::string firstError;
		for (const Record& record : m_Records)
		{
			if (record.Entry.State != PluginState::Rejected)
				continue;
			++rejected;
			if (firstError.empty())
				firstError = record.Entry.Manifest.Id + ": " + record.Entry.Diagnostic;
		}
		if (rejected > 0)
		{
			if (error) *error = firstError.empty() ? "plugin rejected" : firstError;
			return Status::Rejected;
		}
		return Status::Ok;
	}

	PluginManager::Status PluginManager::Load(const std::string& id, WorldContext& context, std::string* error)
	{
		for (size_t index = 0; index < m_Records.size(); ++index)
			if (m_Records[index].Entry.Manifest.Id == id)
				return LoadRecord(index, context, error);
		if (error) *error = "plugin not found: " + id;
		return Status::NotFound;
	}

	PluginManager::Status PluginManager::UnloadRecord(Record& record, WorldContext& context, std::string* error,
		bool enforceDependents)
	{
		const std::string id = record.Entry.Manifest.Id;
		if (record.Entry.State != PluginState::Loaded)
		{
			if (error) *error = "plugin is not loaded: " + id;
			return Status::NotLoaded;
		}

		if (enforceDependents)
		{
			// 被其它已加载插件依赖时拒绝(不静默级联卸载):先卸载依赖者。
			std::string dependents;
			for (const Record& other : m_Records)
			{
				if (&other == &record || other.Entry.State != PluginState::Loaded)
					continue;
				const std::vector<std::string>& depends = other.Entry.Manifest.Depends;
				if (std::find(depends.begin(), depends.end(), id) == depends.end())
					continue;
				if (!dependents.empty())
					dependents += ", ";
				dependents += other.Entry.Manifest.Id;
			}
			if (!dependents.empty())
			{
				if (error) *error = "plugin '" + id + "' is still required by loaded plugin(s): " + dependents;
				return Status::HasLoadedDependents;
			}
		}

		// Unregister 恰好一次(契约);之后才释放 DLL。HostApiBox 在 Unregister 期间保持有效。
		std::string failure;
		if (record.Plugin && record.Plugin->Unregister)
		{
			try
			{
				record.Plugin->Unregister(context);
			}
			catch (const std::exception& exception)
			{
				failure = std::string("plugin Unregister raised an exception: ") + exception.what();
			}
			catch (...)
			{
				failure = "plugin Unregister raised an unknown exception";
			}
		}
		else if (record.Plugin)
		{
			failure = "plugin has no Unregister entry (contract violation)";
		}

		record.Plugin = nullptr;
		record.Host.reset();
		record.Library.reset();
		record.Entry.State = PluginState::Unloaded;
		record.Entry.Order = -1;
		Log(WePluginLogInfo, "unloaded id=" + id);

		if (!failure.empty())
		{
			if (error) *error = failure;
			return Status::Rejected;
		}
		return Status::Ok;
	}

	PluginManager::Status PluginManager::Unload(const std::string& id, WorldContext& context, std::string* error)
	{
		Record* record = FindRecord(id);
		if (!record)
		{
			if (error) *error = "plugin not found: " + id;
			return Status::NotFound;
		}
		return UnloadRecord(*record, context, error, true);
	}

	void PluginManager::UnloadAll(WorldContext& context)
	{
		// 逆序卸载(与 ModuleManager::UnloadAll 同口径):依赖者先走,无需逐个查依赖。
		std::vector<std::string> order = LoadOrder();
		for (auto it = order.rbegin(); it != order.rend(); ++it)
		{
			std::string reason;
			const Status status = Unload(*it, context, &reason);
			if (status != Status::Ok)
				Log(WePluginLogError, "unload failed id=" + *it + " reason=" + reason);
		}
	}
}
