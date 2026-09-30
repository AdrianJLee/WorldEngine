#include "wldpch.h"
#include "World/Plugins/PluginManager.h"
#include "World/Core/Asset/AssetTypeRegistry.h"
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

		// ---- T2:插件注册面 → 宿主既有注册表 / CookPipeline 清单的适配 ------------------

		// 资产类型"新建"回调的宿主侧状态:生命周期 = AssetTypeRegistry 里那条记录;
		// 注销(插件自己注销或卸载兜底回收)时随记录一起销毁,不留悬空回调。
		struct PluginAssetTypeState
		{
			std::string PluginId;
			WeAssetTypeCreateFn Create = nullptr;
			void* UserData = nullptr;
		};

		// 插件导入器 → IAssetImporter 适配器。回调会跳进插件 DLL,所以卸载前必须先从
		// PluginManager 的清单里移除(见 PluginManager::ReclaimPluginRegistrations)。
		// Name() 取稳定 Id(不是 DisplayName):cook 复合指纹用 Name+Version 标识导入器身份,
		// 显示名只做诊断/面板展示。
		class PluginImporterAdapter final : public World::Asset::IAssetImporter
		{
		public:
			PluginImporterAdapter(std::string pluginId, std::string id,
				const WeAssetImporterDesc& desc)
				: m_PluginId(std::move(pluginId))
				, m_Id(std::move(id))
				, m_DisplayName(desc.DisplayName ? desc.DisplayName : m_Id)
				, m_Version(desc.Version)
				, m_UserData(desc.UserData)
				, m_Matches(desc.Matches)
				, m_Import(desc.Import)
				, m_Fingerprint(desc.SettingsFingerprint)
			{
			}

			const std::string& Id() const { return m_Id; }
			const std::string& DisplayName() const { return m_DisplayName; }
			const std::string& PluginId() const { return m_PluginId; }

			std::string Name() const override { return m_Id; }
			uint32_t Version() const override { return m_Version; }

			bool Matches(const std::filesystem::path& source) const override
			{
				if (!m_Matches)
					return false;
				try
				{
					return m_Matches(m_UserData, source.u8string().c_str());
				}
				catch (...)
				{
					return false;   // 契约违约(回调不得抛)= 不接手,不把异常带进 cook
				}
			}

			uint64_t SettingsFingerprint(const std::filesystem::path& source) const override
			{
				if (!m_Fingerprint)
					return 0;
				try
				{
					return m_Fingerprint(m_UserData, source.u8string().c_str());
				}
				catch (...)
				{
					return 0;
				}
			}

			World::Asset::ImportResult Import(const World::Asset::ImportRequest& request,
				std::error_code& ec) const override
			{
				(void)ec;   // 失败原因统一走 ImportResult::Error(与内建导入器同一读法)
				World::Asset::ImportResult result;
				if (!m_Import)
				{
					result.Error = "plugin importer '" + m_Id + "' has no Import callback";
					return result;
				}

				SinkState state;
				state.Result = &result;
				WeImportSink sink;
				sink.StructSize = sizeof(WeImportSink);
				sink.AbiVersion = WE_PLUGIN_ABI_VERSION;
				sink.UserData = &state;
				sink.Write = &PluginImporterAdapter::WriteSink;

				char errorBuffer[512] = {};
				bool ok = false;
				try
				{
					ok = m_Import(m_UserData, request.LogicalPath.c_str(),
						request.Source.u8string().c_str(), &sink, errorBuffer,
						static_cast<uint32_t>(sizeof(errorBuffer)));
				}
				catch (...)
				{
					ok = false;
				}
				errorBuffer[sizeof(errorBuffer) - 1] = '\0';

				if (!ok)
				{
					result.Data.clear();
					result.Outputs.clear();
					result.Error = errorBuffer[0] ? std::string(errorBuffer)
						: ("plugin importer '" + m_Id + "' failed");
					return result;
				}
				if (!state.SingleWritten && !state.MultiWritten)
				{
					result.Error = "plugin importer '" + m_Id
						+ "' reported success without writing a product";
					return result;
				}
				result.Ok = true;
				return result;
			}

		private:
			struct SinkState
			{
				World::Asset::ImportResult* Result = nullptr;
				bool SingleWritten = false;
				bool MultiWritten = false;
			};

			static bool WriteSink(void* userData, const char* logicalPathUtf8,
				const void* bytes, uint64_t size)
			{
				auto* state = static_cast<SinkState*>(userData);
				if (!state || !state->Result || (size > 0 && !bytes))
					return false;

				const bool multi = logicalPathUtf8 && logicalPathUtf8[0];
				if (multi && state->SingleWritten)
					return false;   // 单产物与多产物互斥(ImportResult 契约)
				if (!multi && (state->SingleWritten || state->MultiWritten))
					return false;   // 单产物只能写一次

				std::vector<uint8_t> data;
				if (size > 0)
					data.assign(static_cast<const uint8_t*>(bytes),
						static_cast<const uint8_t*>(bytes) + static_cast<size_t>(size));

				if (multi)
				{
					World::Asset::ImportOutput output;
					output.LogicalPath = logicalPathUtf8;
					output.Data = std::move(data);
					state->Result->Outputs.push_back(std::move(output));
					state->MultiWritten = true;
				}
				else
				{
					state->Result->Data = std::move(data);
					state->SingleWritten = true;
				}
				return true;
			}

			std::string m_PluginId;
			std::string m_Id;
			std::string m_DisplayName;
			uint32_t m_Version = 1;
			void* m_UserData = nullptr;
			WeAssetImporterMatchesFn m_Matches = nullptr;
			WeAssetImportFn m_Import = nullptr;
			WeAssetImporterFingerprintFn m_Fingerprint = nullptr;
		};
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
		// T2 注册面(尾部追加,WePluginApi.h 的 append-only 纪律)。
		m_HostApi.RegisterAssetType = &PluginManager::BridgeRegisterAssetType;
		m_HostApi.UnregisterAssetType = &PluginManager::BridgeUnregisterAssetType;
		m_HostApi.RegisterAssetImporter = &PluginManager::BridgeRegisterAssetImporter;
		m_HostApi.UnregisterAssetImporter = &PluginManager::BridgeUnregisterAssetImporter;
		m_HostApi.LookupExport = &PluginManager::BridgeLookupExport;
	}

	PluginManager::~PluginManager()
	{
		if (LoadedCount() > 0)
			Log(WePluginLogError, "manager destroyed with " + std::to_string(LoadedCount())
				+ " plugin(s) still loaded (host must call UnloadAll first)");
		// T2:管理器析构会让 Library 一起释放。即使宿主违约(没先 UnloadAll),也必须把
		// 指向这些 DLL 的注册回调从宿主注册面移除 —— 否则留下悬空回调。
		for (Record& record : m_Records)
			ReclaimPluginRegistrations(record);
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

	// ---- T2:宿主注册面(WeHostApi 尾部字段的宿主实现)----------------------------------

	PluginManager::Record* PluginManager::ResolveHostRecord(HostApiBox& box)
	{
		if (box.Manager != this)
			return nullptr;
		Record* record = FindRecord(box.PluginId);
		if (!record)
			return nullptr;
		// 只接受:本管理器在 Register 调用期间交给插件的那张表,或该条目自己的表(Unregister 期间)。
		if (m_ActiveBox != &box && record->Host.get() != &box)
			return nullptr;
		return record;
	}

	bool PluginManager::BridgeRegisterAssetType(void* userData, const WeAssetTypeDesc* desc)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": asset type registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterAssetType(*record, *desc);
	}

	bool PluginManager::BridgeUnregisterAssetType(void* userData, const char* id)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": asset type unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterAssetType(*record, id);
	}

	bool PluginManager::BridgeRegisterAssetImporter(void* userData, const WeAssetImporterDesc* desc)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": importer registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterAssetImporter(*record, *desc);
	}

	bool PluginManager::BridgeUnregisterAssetImporter(void* userData, const char* id)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": importer unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterAssetImporter(*record, id);
	}

	void* PluginManager::BridgeLookupExport(void* userData, const char* pluginId, const char* name,
		uint32_t minVersion)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager || !pluginId || !name)
			return nullptr;
		return box->Manager->LookupExport(pluginId, name, minVersion);
	}

	bool PluginManager::RegisterAssetType(Record& record, const WeAssetTypeDesc& desc)
	{
		const std::string& pluginId = record.Entry.Manifest.Id;

		// 前缀校验:宿主要读 v1 全字段 ⇒ 声明的大小必须覆盖它;ABI 等值门同 WePlugin。
		if (desc.StructSize < sizeof(WeAssetTypeDesc) || desc.AbiVersion != WE_PLUGIN_ABI_VERSION)
		{
			Log(WePluginLogWarn, pluginId + ": asset type registration rejected (struct/abi mismatch size="
				+ std::to_string(desc.StructSize) + " abi=" + std::to_string(desc.AbiVersion) + ")");
			return false;
		}
		const std::string id = desc.Id ? desc.Id : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": asset type registration rejected (empty id)");
			return false;
		}

		World::AssetTypeRegistry& registry = World::AssetTypeRegistry::Get();
		if (registry.Find(id))
		{
			// 重复 id = 不覆盖(注册方必须处理返回值);可能是别的所有者,也可能是本插件第二次。
			Log(WePluginLogWarn, pluginId + ": asset type '" + id
				+ "' is already registered; registration ignored");
			return false;
		}

		World::AssetTypeDesc converted;
		converted.Id = id;
		converted.Label = desc.Label && desc.Label[0] ? desc.Label : id;
		converted.Term = desc.Term && desc.Term[0] ? desc.Term : converted.Label;
		converted.Extension = desc.Extension ? desc.Extension : "";
		converted.Icon = desc.Icon;
		converted.SortOrder = desc.SortOrder;
		converted.IsFolder = desc.IsFolder != 0;
		if (desc.Create)
		{
			auto state = std::make_shared<PluginAssetTypeState>();
			state->PluginId = pluginId;
			state->Create = desc.Create;
			state->UserData = desc.UserData;
			converted.Create = [state](const std::filesystem::path& dir, std::string* error) -> bool
			{
				char errorBuffer[512] = {};
				bool ok = false;
				try
				{
					ok = state->Create(state->UserData, dir.u8string().c_str(), errorBuffer,
						static_cast<uint32_t>(sizeof(errorBuffer)));
				}
				catch (...)
				{
					ok = false;   // 契约违约(回调不得抛):当成"新建失败",不把异常带进编辑器
				}
				errorBuffer[sizeof(errorBuffer) - 1] = '\0';
				if (!ok && error)
					*error = errorBuffer[0] ? std::string(errorBuffer)
						: ("plugin asset type '" + state->PluginId + "' could not create the asset");
				return ok;
			};
		}

		registry.Register(std::move(converted));
		record.RegisteredAssetTypes.push_back(id);
		Log(WePluginLogInfo, pluginId + ": registered asset type '" + id + "'");
		return true;
	}

	bool PluginManager::UnregisterAssetType(Record& record, const char* rawId)
	{
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string id = rawId ? rawId : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": asset type unregister rejected (empty id)");
			return false;
		}

		const auto tracked = std::find(record.RegisteredAssetTypes.begin(),
			record.RegisteredAssetTypes.end(), id);
		if (tracked == record.RegisteredAssetTypes.end())
		{
			// 幂等口径:本插件没注册过 → true 且不报错;但别人的类型必须拒绝(不能顺手删掉)。
			if (World::AssetTypeRegistry::Get().Find(id))
			{
				Log(WePluginLogWarn, pluginId + ": asset type '" + id
					+ "' is not owned by this plugin; unregister ignored");
				return false;
			}
			return true;
		}

		record.RegisteredAssetTypes.erase(tracked);
		World::AssetTypeRegistry::Get().Unregister(id);
		Log(WePluginLogInfo, pluginId + ": unregistered asset type '" + id + "'");
		return true;
	}

	bool PluginManager::RegisterAssetImporter(Record& record, const WeAssetImporterDesc& desc)
	{
		const std::string& pluginId = record.Entry.Manifest.Id;
		if (desc.StructSize < sizeof(WeAssetImporterDesc) || desc.AbiVersion != WE_PLUGIN_ABI_VERSION)
		{
			Log(WePluginLogWarn, pluginId + ": importer registration rejected (struct/abi mismatch size="
				+ std::to_string(desc.StructSize) + " abi=" + std::to_string(desc.AbiVersion) + ")");
			return false;
		}
		const std::string id = desc.Id ? desc.Id : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": importer registration rejected (empty id)");
			return false;
		}
		if (!desc.Matches || !desc.Import)
		{
			Log(WePluginLogWarn, pluginId + ": importer '" + id
				+ "' registration rejected (Matches/Import callback missing)");
			return false;
		}
		for (const Record& other : m_Records)
		{
			for (const RegisteredImporter& item : other.RegisteredImporters)
			{
				if (item.Id == id)
				{
					Log(WePluginLogWarn, pluginId + ": importer '" + id
						+ "' is already registered (owner=" + other.Entry.Manifest.Id
						+ "); registration ignored");
					return false;
				}
			}
		}

		RegisteredImporter item;
		item.Id = id;
		item.Importer = std::make_shared<PluginImporterAdapter>(pluginId, id, desc);
		record.RegisteredImporters.push_back(std::move(item));
		Log(WePluginLogInfo, pluginId + ": registered importer '" + id + "'");
		return true;
	}

	bool PluginManager::UnregisterAssetImporter(Record& record, const char* rawId)
	{
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string id = rawId ? rawId : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": importer unregister rejected (empty id)");
			return false;
		}

		auto tracked = std::find_if(record.RegisteredImporters.begin(),
			record.RegisteredImporters.end(),
			[&id](const RegisteredImporter& item) { return item.Id == id; });
		if (tracked == record.RegisteredImporters.end())
		{
			for (const Record& other : m_Records)
			{
				if (&other == &record)
					continue;
				for (const RegisteredImporter& item : other.RegisteredImporters)
				{
					if (item.Id == id)
					{
						Log(WePluginLogWarn, pluginId + ": importer '" + id
							+ "' is not owned by this plugin; unregister ignored");
						return false;
					}
				}
			}
			return true;   // 幂等:没注册过 → true,不报错
		}

		record.RegisteredImporters.erase(tracked);
		Log(WePluginLogInfo, pluginId + ": unregistered importer '" + id + "'");
		return true;
	}

	void* PluginManager::LookupExport(const std::string& pluginId, const std::string& name,
		uint32_t minVersion) const
	{
		const Record* record = FindRecord(pluginId);
		if (!record || record->Entry.State != PluginState::Loaded || !record->Plugin)
			return nullptr;
		const WePlugin& plugin = *record->Plugin;
		for (uint32_t index = 0; index < plugin.ExportCount && plugin.Exports; ++index)
		{
			const WePluginExport& item = plugin.Exports[index];
			if (!item.Name || !item.Function)
				continue;
			if (name == item.Name && item.Version >= minVersion)
				return item.Function;
		}
		return nullptr;
	}

	std::vector<std::shared_ptr<World::Asset::IAssetImporter>> PluginManager::PluginImporters() const
	{
		std::vector<std::shared_ptr<World::Asset::IAssetImporter>> importers;
		for (const Record& record : m_Records)
			for (const RegisteredImporter& item : record.RegisteredImporters)
				importers.push_back(item.Importer);
		return importers;
	}

	void PluginManager::ReclaimPluginRegistrations(Record& record)
	{
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
		host->Manager = this;
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
			ReclaimPluginRegistrations(record);
			if (error) *error = registerFailure;
			return Status::Rejected;
		}
		m_ActiveBox = nullptr;

		if (!registered)
		{
			// 契约:Register 返回 false = 插件自己已清干净,宿主不调用 Unregister(防二次释放);
			// 兜底仍回收它留下的注册项(违约插件也不留悬空回调)。
			ReclaimPluginRegistrations(record);
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

		// T2 兜底:Unregister 之后,插件没自己注销的资产类型/导入器在这里回收
		// (仍在注册表里的回调指向本 DLL,必须在释放 DLL 之前移除)。
		ReclaimPluginRegistrations(record);

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
