#include "PluginManager_Internal.h"

namespace World::Plugins
{

using namespace PluginManagerDetail;


	// ---- HOTR-P3-T8:插件"一键重载"的请求队列 / 定位 / 构建进度 --------------------

void PluginManager::RequestReloadWithBuild(const std::string& id){
		if (id.empty())
			return;
		// 与 RequestReload 同一个请求队列:登记顺序 + 按 id 去重由它统一保证。
		RequestReload(id);
		if (std::find(m_ReloadBuildRequests.begin(), m_ReloadBuildRequests.end(), id)
			== m_ReloadBuildRequests.end())
			m_ReloadBuildRequests.push_back(id);
	}


std::vector<PluginManager::PluginReloadRequest> PluginManager::ConsumeReloadRequestsDetailed(){
		const std::vector<std::string> ids = ConsumeReloadRequests();
		std::vector<PluginReloadRequest> requests;
		requests.reserve(ids.size());
		for (const std::string& id : ids)
		{
			PluginReloadRequest request;
			request.Id = id;
			request.Build = std::find(m_ReloadBuildRequests.begin(), m_ReloadBuildRequests.end(), id)
				!= m_ReloadBuildRequests.end();
			requests.push_back(std::move(request));
		}
		m_ReloadBuildRequests.clear();
		return requests;
	}


std::filesystem::path PluginManager::PluginSourceDir(const std::string& id) const{
		const Record* record = FindRecord(id);
		return record ? record->Entry.Manifest.Root : std::filesystem::path();
	}


std::string PluginManager::PluginBuildTarget(const std::string& id) const{
		const Record* record = FindRecord(id);
		if (!record)
			return std::string();
		// 模板约定 = "WePlugin_" + 插件目录名(templates/plugin-*/CMakeLists.txt 的
		// add_library(WePlugin_{{PluginDir}} ...);目录名不可得 = 空串)。
		const std::string directoryName = record->Entry.Manifest.Root.filename().string();
		if (directoryName.empty())
			return std::string();
		return "WePlugin_" + directoryName;
	}


void PluginManager::BeginPluginBuild(const std::string& id){
		for (auto& [otherId, other] : m_PluginBuilds)
		{
			if (otherId != id)
				other.Running = false;   // 防御:构建器同一时刻只跑一个,旧的 Running 不残留
		}
		PluginBuildProgress& progress = m_PluginBuilds[id];
		progress.Running = true;
		progress.ExitCode = -1;
		progress.Output.clear();
	}


void PluginManager::CompletePluginBuild(const std::string& id, int exitCode, const std::string& output){
		PluginBuildProgress& progress = m_PluginBuilds[id];
		progress.Running = false;
		progress.ExitCode = exitCode;
		progress.Output = output;
	}


PluginManager::PluginBuildProgress PluginManager::PluginBuildState(const std::string& id) const{
		const auto found = m_PluginBuilds.find(id);
		return found == m_PluginBuilds.end() ? PluginBuildProgress() : found->second;
	}


bool PluginManager::PluginBuildRunning() const{
		for (const auto& entry : m_PluginBuilds)
			if (entry.second.Running)
				return true;
		return false;
	}


void PluginManager::ValidateDependencies(){
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


std::string PluginManager::CycleChain(const std::vector<Record>& records, const std::vector<size_t>& nodes){
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


PluginManager::Status PluginManager::LoadRecord(size_t index, WorldContext& context, std::string* error){
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
		// PLUG-CLEAN-1:插件产物可能被外部重编/损坏(热重载第二段就按设计加载坏 DLL 走回滚),
		// 而 `LoadLibraryA` 遇到坏镜像会走 Windows 的 hard-error 路径:进程默认错误模式里
		// 没有 `SEM_FAILCRITICALERRORS` / `SEM_NOOPENFILEERRORBOX` 时,加载会**阻塞在
		// 系统错误框**上(实测:ctest 子进程默认错误模式 = 0 ⇒ World.Plugins 卡死超时;
		// PowerShell 子进程 = 0x8001 ⇒ 立即返回 193)。"加载失败 = 干净拒绝"是插件契约,
		// 所以只在这一次加载窗口内压制系统错误框,随后恢复进程原错误模式。
		const UINT previousErrorMode = ::SetErrorMode(
			SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
		const bool loaded = library->Load(libraryPath.string());
		::SetErrorMode(previousErrorMode);
		if (!loaded)
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
		// 导出表名(面板/plugin.info 展示用;在契约校验之前就记下来,失败条目也能诊断)。
		entry.ExportNames.clear();
		for (uint32_t i = 0; i < plugin->ExportCount && plugin->Exports; ++i)
			if (plugin->Exports[i].Name && plugin->Exports[i].Name[0])
				entry.ExportNames.push_back(plugin->Exports[i].Name);
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
		host->Manager = this;
		// T2b:组件 schema 注册到本次加载的 WorldContext 的注册表(插件经 WeHostApi 回调时
		// 只拿得到本盒子,所以归属在这里记住)。
		host->Schemas = &context.Schemas();
		// T6:单类型注销的活实例门按本次加载的 WorldContext 过滤(与 Unload 同口径)。
		host->Context = &context;
		host->Api = m_HostApi;
		host->Api.UserData = host.get();

		bool registered = false;
		std::string registerFailure;
		m_ActiveBox = host.get();
		try
		{
			registered = plugin->Register(context, host->Api);
		}
		catch (const std::exception& exception)
		{
			registerFailure = std::string("plugin registration raised an exception: ") + exception.what();
		}
		catch (...)
		{
			registerFailure = "plugin registration raised an unknown exception";
		}

		if (!registerFailure.empty())
		{
			// 契约违约(回调不该抛):best-effort Unregister 回滚可能留下的半注册状态。
			// m_ActiveBox 保持有效 ⇒ 回滚里的 host.UnregisterAssetType/… 仍能按归属注销。
			if (plugin->Unregister)
			{
				try { plugin->Unregister(context); }
				catch (...) {}
			}
			m_ActiveBox = nullptr;
			// T2b:record.Host 此刻还没建立,注册表句柄从本次调用的 context 取。
			ReclaimPluginRegistrations(record, &context.Schemas());
			if (error) *error = registerFailure;
			return Status::Rejected;
		}
		m_ActiveBox = nullptr;

		if (!registered)
		{
			// 契约:Register 返回 false = 插件自己已清干净,宿主不调用 Unregister(防二次释放);
			// 兜底仍回收它留下的注册项(违约插件也不留悬空回调)。
			ReclaimPluginRegistrations(record, &context.Schemas());
			if (error) *error = "plugin registration failed (Register returned false)";
			return Status::Rejected;
		}

		record.Library = std::move(library);
		record.Host = std::move(host);
		record.Plugin = plugin;
		record.LoadedLibraryPath = libraryPath;
		entry.State = PluginState::Loaded;
		entry.Order = m_NextOrder++;
		entry.Diagnostic.clear();
		Log(WePluginLogInfo, "loaded id=" + entry.Manifest.Id + " scope="
			+ PluginScopeName(entry.Manifest.Scope) + " order=" + std::to_string(entry.Order));
		// PLUG-T5:声明(contributes)与实际注册的一致性 —— 不一致会让 cook 的引用索引漏报/误报。
		WarnContributionDrift(record);
		return Status::Ok;
	}


PluginManager::Status PluginManager::LoadAll(WorldContext& context, std::string* error){
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


PluginManager::Status PluginManager::Load(const std::string& id, WorldContext& context, std::string* error){
		for (size_t index = 0; index < m_Records.size(); ++index)
			if (m_Records[index].Entry.Manifest.Id == id)
				return LoadRecord(index, context, error);
		if (error) *error = "plugin not found: " + id;
		return Status::NotFound;
	}


	// PLUG-T5:发行形态加载(见 PluginManager.h 的契约说明)。
PluginManager::Status PluginManager::LoadPackaged(const std::filesystem::path& libraryDir, const std::vector<std::string>& orderedIds, WorldContext& context, std::string* error){
		if (orderedIds.empty())
			return Status::Ok;   // 发行清单没带插件 = 零插件、零开销(与"缺根 = 0 个插件"同口径)
		if (LoadedCount() > 0 || !m_Records.empty())
		{
			Log(WePluginLogError, "packaged load refused: the manager is not empty (call UnloadAll first)");
			if (error) *error = "plugin manager already holds entries";
			return Status::Rejected;
		}

		m_Records.clear();
		m_LoadSequence.clear();
		m_NextOrder = 0;

		// 1. 平铺目录扫描:每个 DLL 只做"读 id"的探针(LoadLibrary + WePluginQuery),
		//    不做 Register —— 真正的加载统一走 LoadRecord(同一套契约校验与回滚)。
		std::map<std::string, std::filesystem::path> libraryById;
		std::error_code ec;
		if (std::filesystem::is_directory(libraryDir, ec))
		{
			std::vector<std::filesystem::path> libraries;
			for (const std::filesystem::directory_entry& item :
				std::filesystem::directory_iterator(libraryDir, ec))
			{
				std::error_code fileEc;
				if (item.is_regular_file(fileEc)
					&& item.path().extension().string() == kPluginLibraryExtension)
					libraries.push_back(item.path());
			}
			std::sort(libraries.begin(), libraries.end());

			for (const std::filesystem::path& libraryPath : libraries)
			{
				auto library = std::make_unique<World::DynamicLibrary>();
				if (!library->Load(libraryPath.string()))
				{
					Log(WePluginLogError, "packaged plugin library failed to load: " + libraryPath.string()
						+ " (" + library->GetLastError() + ")");
					continue;
				}
				const auto query = reinterpret_cast<WePluginQueryFn>(
					library->GetSymbol(WE_PLUGIN_QUERY_SYMBOL));
				if (!query)
				{
					Log(WePluginLogError, "packaged plugin library has no '" + std::string(WE_PLUGIN_QUERY_SYMBOL)
						+ "' entry: " + libraryPath.string());
					continue;
				}
				const WePlugin* plugin = query(WE_PLUGIN_ABI_VERSION);
				if (!plugin)
				{
					Log(WePluginLogError, "packaged plugin rejected the host plugin ABI "
						+ std::to_string(WE_PLUGIN_ABI_VERSION) + ": " + libraryPath.string());
					continue;
				}
				const std::string id = plugin->Id ? plugin->Id : "";
				if (id.empty())
				{
					Log(WePluginLogError, "packaged plugin has an empty id: " + libraryPath.string());
					continue;
				}
				const auto existing = libraryById.find(id);
				if (existing != libraryById.end())
				{
					Log(WePluginLogWarn, "packaged plugin id '" + id + "' appears twice in "
						+ libraryDir.string() + " (" + existing->second.string() + ", "
						+ libraryPath.string() + "); keeping the first");
					continue;
				}
				libraryById.emplace(id, libraryPath);
			}
		}
		else
		{
			Log(WePluginLogError, "packaged plugin directory not found: " + libraryDir.string()
				+ " (the release manifest lists " + std::to_string(orderedIds.size()) + " plugin(s))");
		}

		// 2. 按发行清单的顺序(= cook 写下的依赖拓扑序)逐条加载;单条失败不阻断其余。
		std::string firstError;
		size_t failed = 0;
		for (const std::string& id : orderedIds)
		{
			const auto found = libraryById.find(id);
			if (found == libraryById.end())
			{
				++failed;
				const std::string reason = "release manifest ships plugin '" + id + "' but "
					+ libraryDir.string() + " has no library providing it";
				Log(WePluginLogError, reason);
				if (firstError.empty())
					firstError = reason;
				continue;
			}

			Record record;
			record.Entry.Manifest.Id = id;
			record.Entry.Manifest.Name = id;
			record.Entry.Manifest.Scope = PluginScope::Engine;   // 打包形态没有"项目/引擎"之分
			record.Entry.Manifest.LibraryPath = found->second;
			record.Entry.Manifest.ManifestPath = found->second;
			m_Records.push_back(std::move(record));
			const size_t index = m_Records.size() - 1;

			std::string reason;
			const Status status = LoadRecord(index, context, &reason);
			if (status == Status::Ok)
				continue;
			++failed;
			if (reason.empty())
				reason = StatusName(status);
			Reject(m_Records[index], reason);
			if (firstError.empty())
				firstError = "plugin '" + id + "': " + reason;
		}

		// 3. 目录里多余的 DLL(清单没列)⇒ 警告后忽略(不静默)。
		for (const auto& entry : libraryById)
		{
			if (std::find(orderedIds.begin(), orderedIds.end(), entry.first) != orderedIds.end())
				continue;
			Log(WePluginLogWarn, "packaged plugin '" + entry.first + "' is present in "
				+ libraryDir.string() + " but is not listed in the release manifest; ignored");
		}

		if (failed > 0)
		{
			if (error) *error = firstError;
			return Status::Rejected;
		}
		return Status::Ok;
	}


PluginManager::Status PluginManager::UnloadRecord(Record& record, WorldContext& context, std::string* error, bool enforceDependents){
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

		// ---- T2c:活实例门(卸载前置检查)-------------------------------------------
		// 插件的 blob 组件还有实例挂在活场景实体上(同一 WorldContext)⇒ **干净拒绝**:
		// 注销 schema 会把那些实例变成"没有 schema 的孤儿"(序列化会直接丢数据)。
		// 先移除组件 / 销毁场景,再重试 Unload。
		{
			std::string liveReport;
			std::size_t liveTotal = 0;
			for (const RegisteredComponent& item : record.RegisteredComponents)
			{
				if (!item.Schema.Storage)
					continue;   // schema-only 组件没有实例可挂
				const std::size_t instances = World::Scene::CountLiveComponentInstances(
					item.Schema.Storage->ComponentId, &context);
				if (instances == 0)
					continue;
				liveTotal += instances;
				if (!liveReport.empty())
					liveReport += ", ";
				liveReport += item.Id + " x" + std::to_string(instances);
			}
			if (liveTotal > 0)
			{
				const std::string reason = "plugin '" + id + "' still has live component instances ("
					+ liveReport + "); remove the components or destroy the scene before unloading";
				Log(WePluginLogWarn, reason);
				if (error) *error = reason;
				return Status::HasLiveInstances;
			}
		}

		// Unregister 恰好一次(契约);之后才释放 DLL。HostApiBox 在 Unregister 期间保持有效。
		std::string failure;
		// T2b:插件可能在 Unregister 里注销自己注册的组件类型 —— 先刷新注册表归属。
		if (record.Host)
		{
			record.Host->Schemas = &context.Schemas();
			record.Host->Context = &context;
		}
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

		// T2/T2b 兜底:Unregister 之后,插件没自己注销的资产类型/导入器/组件类型在这里回收
		// (仍在注册表里的回调指向本 DLL,必须在释放 DLL 之前移除)。
		ReclaimPluginRegistrations(record, &context.Schemas());

		record.Plugin = nullptr;
		record.Host.reset();
		record.Library.reset();
		record.LoadedLibraryPath.clear();
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


PluginManager::Status PluginManager::Unload(const std::string& id, WorldContext& context, std::string* error){
		Record* record = FindRecord(id);
		if (!record)
		{
			if (error) *error = "plugin not found: " + id;
			return Status::NotFound;
		}
		return UnloadRecord(*record, context, error, true);
	}


void PluginManager::UnloadAll(WorldContext& context){
		// 逆序卸载(与 ModuleManager::UnloadAll 同口径):依赖者先走,无需逐个查依赖。
		std::vector<std::string> order = LoadOrder();
		for (auto it = order.rbegin(); it != order.rend(); ++it)
		{
			std::string reason;
			const Status status = Unload(*it, context, &reason);
			if (status == Status::HasLiveInstances)
				Log(WePluginLogWarn, "unload skipped (live component instances) id=" + *it
					+ " reason=" + reason);
			else if (status != Status::Ok)
				Log(WePluginLogError, "unload failed id=" + *it + " reason=" + reason);
		}
		// T6:UnloadAll 是"收工"路径 —— 还有没写回的实例快照 = 数据丢失(记 ERROR 后清掉,
		// 避免把过期快照带进下一次会话)。
		for (const auto& [id, pending] : m_PendingReloads)
			Log(WePluginLogError, "unload-all dropped pending reload state for plugin '" + id + "' ("
				+ std::to_string(pending.InstancesSnapshotted) + " snapshotted instance(s) lost)");
		m_PendingReloads.clear();
	}

}
