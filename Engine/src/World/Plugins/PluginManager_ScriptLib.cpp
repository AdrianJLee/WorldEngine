#include "PluginManager_Internal.h"

namespace World::Plugins
{

using namespace PluginManagerDetail;


	// ---- T4:脚本函数库(账本在 World/Script/PluginScriptLibrary)------------------------

bool PluginManager::RegisterScriptFunction(Record& record, const WeScriptFunctionDesc& desc){
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string name = desc.Name ? desc.Name : "";
		if (name.empty())
		{
			Log(WePluginLogWarn, pluginId + ": script function registration rejected (empty name)");
			return false;
		}
		std::string failure;
		if (!World::PluginScriptLibrary::Register(pluginId, desc, &failure))
		{
			Log(WePluginLogWarn, pluginId + ": script function '" + name
				+ "' registration rejected (" + failure + ")");
			return false;
		}
		record.RegisteredScriptFunctions.push_back(name);
		Log(WePluginLogInfo, pluginId + ": registered script function '" + name + "'");
		return true;
	}


bool PluginManager::UnregisterScriptFunction(Record& record, const char* name){
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string id = name ? name : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": script function unregister rejected (empty name)");
			return false;
		}
		const auto tracked = std::find(record.RegisteredScriptFunctions.begin(),
			record.RegisteredScriptFunctions.end(), id);
		if (tracked == record.RegisteredScriptFunctions.end())
		{
			// 幂等路径:名字归别的插件 = 拒绝(不越权);否则 = true(可能已被兜底回收)。
			const std::string owner = World::PluginScriptLibrary::OwnerOf(id);
			if (!owner.empty() && owner != pluginId)
			{
				Log(WePluginLogWarn, pluginId + ": script function '" + id
					+ "' is not owned by this plugin; unregister ignored");
				return false;
			}
			return true;
		}
		std::string failure;
		if (!World::PluginScriptLibrary::Unregister(pluginId, id, &failure))
		{
			Log(WePluginLogWarn, pluginId + ": script function '" + id
				+ "' unregister failed (" + failure + ")");
			return false;
		}
		record.RegisteredScriptFunctions.erase(tracked);
		Log(WePluginLogInfo, pluginId + ": unregistered script function '" + id + "'");
		return true;
	}


void PluginManager::ReclaimEditorExtensions(Record& record){
		const std::string& pluginId = record.Entry.Manifest.Id;
		for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
		{
			Log(WePluginLogWarn, pluginId + ": editor command '" + item.Id
				+ "' was not unregistered by the plugin; force-removed");
			if (m_EditorHost)
				m_EditorHost->UnregisterEditorCommand(pluginId, item.Id);
		}
		record.RegisteredEditorCommands.clear();
		for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
		{
			Log(WePluginLogWarn, pluginId + ": editor panel '" + item.Id
				+ "' was not unregistered by the plugin; force-removed");
			if (m_EditorHost)
				m_EditorHost->UnregisterEditorPanel(pluginId, item.Id);
		}
		record.RegisteredEditorPanels.clear();
	}


int PluginManager::RenderPanelEntry(void* host, const PluginEditorPanel& panel, WeEditorUiApi* ui, void* uiContext, void* userData){
		// userData = 拥有该面板的 Record(卸载/回滚会先行注销,所以这里必然还活着)。
		// 再按 (pluginId, id) 回查一次:即使宿主留了一条陈旧登记,也不会跳到已释放的 DLL。
		auto* manager = static_cast<PluginManager*>(host);
		Record* record = manager ? manager->FindRecord(panel.PluginId) : nullptr;
		if (!record)
			return -1;
		for (const RegisteredEditorPanel& item : record->RegisteredEditorPanels)
		{
			if (item.Id != panel.Id || !item.Draw)
				continue;
			try
			{
				return item.Draw(userData, ui, uiContext);
			}
			catch (const std::exception& exception)
			{
				Log(WePluginLogError, panel.PluginId + ": editor panel '" + panel.Id
					+ "' draw raised an exception: " + exception.what());
				return -1;
			}
			catch (...)
			{
				Log(WePluginLogError, panel.PluginId + ": editor panel '" + panel.Id
					+ "' draw raised an unknown exception");
				return -1;
			}
		}
		return -1;
	}


	// PLUG-T5:声明与运行时注册的一致性比对(只对**声明了 contributes** 的插件生效)。
	//
	// 为什么重要:cook 的引用完整性硬门用 `contributes:` 做索引,声明漏了 = 引用漏报(静默),
	// 声明多了 = 误报。所以两面不一致必须在插件加载时就以 WARN 暴露出来,而不是等打包时猜。
void PluginManager::WarnContributionDrift(const Record& record){
		const std::vector<PluginContribution>& declared = record.Entry.Manifest.Contributions;
		if (declared.empty())
			return;   // 旧插件/不声明贡献 = 打包索引不覆盖它,这里不打扰

		const std::string& pluginId = record.Entry.Manifest.Id;
		std::set<std::string> declaredComponents;
		std::set<std::string> declaredAssetTypes;
		std::set<std::string> declaredImporters;
		std::set<std::string> declaredNamespaces;
		for (const PluginContribution& contribution : declared)
		{
			switch (contribution.Face)
			{
				case PluginContributionFace::Component: declaredComponents.insert(contribution.Id); break;
				case PluginContributionFace::AssetType: declaredAssetTypes.insert(contribution.Id); break;
				case PluginContributionFace::Importer: declaredImporters.insert(contribution.Id); break;
				case PluginContributionFace::ScriptNamespace: declaredNamespaces.insert(contribution.Id); break;
			}
		}

		std::set<std::string> actualComponents;
		std::set<std::string> actualAssetTypes;
		std::set<std::string> actualImporters;
		std::set<std::string> actualNamespaces;
		for (const RegisteredComponent& component : record.RegisteredComponents)
			actualComponents.insert(component.Id);
		for (const std::string& id : record.RegisteredAssetTypes)
			actualAssetTypes.insert(id);
		for (const RegisteredImporter& importer : record.RegisteredImporters)
			actualImporters.insert(importer.Id);
		for (const std::string& name : record.RegisteredScriptFunctions)
		{
			const size_t dot = name.find('.');
			if (dot != std::string::npos)
				actualNamespaces.insert(name.substr(0, dot));
		}

		const auto compare = [&](const char* face, const std::set<std::string>& declaredIds,
			const std::set<std::string>& actualIds)
		{
			for (const std::string& id : declaredIds)
				if (actualIds.count(id) == 0)
					Log(WePluginLogWarn, "warning id=" + pluginId + " contributes '" + face + ":" + id
						+ "' but did not register it (the cook reference index would be wrong)");
			for (const std::string& id : actualIds)
				if (declaredIds.count(id) == 0)
					Log(WePluginLogWarn, "warning id=" + pluginId + " registered " + face + " '" + id
						+ "' without declaring it under contributes (cook cannot see that reference)");
		};
		compare("component", declaredComponents, actualComponents);
		compare("asset.type", declaredAssetTypes, actualAssetTypes);
		compare("asset.importer", declaredImporters, actualImporters);
		compare("script.namespace", declaredNamespaces, actualNamespaces);
	}


void PluginManager::ReclaimPluginRegistrations(Record& record, World::Schema::SchemaRegistry* schemas){
		const std::string& pluginId = record.Entry.Manifest.Id;
		for (const std::string& id : record.RegisteredAssetTypes)
		{
			if (World::AssetTypeRegistry::Get().Unregister(id))
				Log(WePluginLogWarn, pluginId + ": asset type '" + id
					+ "' was not unregistered by the plugin; force-removed");
		}
		record.RegisteredAssetTypes.clear();
		for (const RegisteredImporter& item : record.RegisteredImporters)
			Log(WePluginLogWarn, pluginId + ": importer '" + item.Id
				+ "' was not unregistered by the plugin; force-removed");
		record.RegisteredImporters.clear();
		// T2b:组件类型整模块注销(在释放 DLL 之前;访问器是宿主侧的,但 schema 不能留在
		// 注册表里让编辑器/序列化继续看到一个已卸载插件的类型)。
		ReclaimComponentTypes(record, schemas);
		// T3b:编辑器命令 / 面板的兜底回收(必须在释放 DLL 之前;登记里的回调指向插件)。
		ReclaimEditorExtensions(record);
		// T4:脚本函数的兜底回收(账本条目 + VM 全局表成员;回调同样指向插件本 DLL)。
		if (!record.RegisteredScriptFunctions.empty())
		{
			for (const std::string& name : record.RegisteredScriptFunctions)
				Log(WePluginLogWarn, pluginId + ": script function '" + name
					+ "' was not unregistered by the plugin; force-removed");
			World::PluginScriptLibrary::UnregisterAll(pluginId);
			record.RegisteredScriptFunctions.clear();
		}
	}


void PluginManager::Reject(Record& record, std::string reason){
		record.Entry.State = PluginState::Rejected;
		record.Entry.Diagnostic = std::move(reason);
		record.Entry.Order = -1;
		Log(WePluginLogError, "rejected id=" + record.Entry.Manifest.Id + " reason=" + record.Entry.Diagnostic);
	}


bool PluginManager::Discover(const std::filesystem::path& enginePluginsRoot, const std::filesystem::path& projectPluginsRoot, const std::vector<std::filesystem::path>& devBinaryRoots){
		if (LoadedCount() > 0)
		{
			Log(WePluginLogError, "discover refused: " + std::to_string(LoadedCount())
				+ " plugin(s) still loaded (call UnloadAll first)");
			return false;
		}

		m_Records.clear();
		m_LoadSequence.clear();
		m_NextOrder = 0;
		m_DevBinaryRoots = devBinaryRoots;
		// T6:重新发现 = 丢掉进程内还没写回的实例快照(数据丢失,必须可观测)。
		for (const auto& [id, pending] : m_PendingReloads)
			Log(WePluginLogError, "discover dropped pending reload state for plugin '" + id + "' ("
				+ std::to_string(pending.InstancesSnapshotted) + " snapshotted instance(s) lost)");
		m_PendingReloads.clear();

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


void PluginManager::ScanRoot(const std::filesystem::path& root, PluginScope scope){
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
			else
			{
				// PLUG-CLEAN-2:`engine:` 最低版本约束 = **硬拒绝门**。约束不满足 ⇒
				// 干净拒绝(Rejected + 可读诊断,不进 loaded、不注册任何面);宿主版本
				// 事实源 = 根 CMakeLists 的 project(World VERSION …)(见 PluginManifest.h)。
				// 清单不带 engine: = 照旧通过。比较结果记在条目上(面板/`plugin.info` 可读)。
				record.Entry.EngineSatisfied = record.Entry.Manifest.Engine.empty()
					|| HostEngineSatisfies(record.Entry.Manifest.EngineMinMajor,
						record.Entry.Manifest.EngineMinMinor);
				if (!record.Entry.EngineSatisfied)
				{
					Reject(record, "engine requirement '" + record.Entry.Manifest.Engine
						+ "' is not satisfied by host engine " + HostEngineVersion());
				}
				else
				{
					// 产物定位(2026-09-30):自带 bin/ 优先;否则在宿主的开发构建根里找同名 DLL
					// (引擎插件 = 引擎构建产出,源码树不写产物)。都没找到就保持自带路径,
					// 由 Load 给出可读的"产物缺失"诊断。
					const std::filesystem::path packaged = record.Entry.Manifest.LibraryPath;
					if (!std::filesystem::is_regular_file(packaged, ec))
					{
						const std::string name = directory.filename().string();
						for (const std::filesystem::path& devRoot : m_DevBinaryRoots)
						{
							const std::filesystem::path candidate = devRoot / (name + kPluginLibraryExtension);
							if (std::filesystem::is_regular_file(candidate, ec))
							{
								record.Entry.Manifest.LibraryPath = candidate;
								Log(WePluginLogInfo, "library resolved from dev build root id="
									+ record.Entry.Manifest.Id + " path=" + candidate.string());
								break;
							}
						}
					}
				}
				ec.clear();
			}
			m_Records.push_back(std::move(record));
		}
	}

}
