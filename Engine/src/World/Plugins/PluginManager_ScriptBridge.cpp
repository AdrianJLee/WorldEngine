#include "PluginManager_Internal.h"

namespace World::Plugins
{

using namespace PluginManagerDetail;


	// ---- T4:脚本函数库桥 ----------------------------------------------------------------

bool PluginManager::BridgeRegisterScriptFunction(void* userData, const WeScriptFunctionDesc* desc){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": script function registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterScriptFunction(*record, *desc);
	}


bool PluginManager::BridgeUnregisterScriptFunction(void* userData, const char* name){
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": script function unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterScriptFunction(*record, name);
	}


bool PluginManager::RegisterAssetType(Record& record, const WeAssetTypeDesc& desc){
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


bool PluginManager::UnregisterAssetType(Record& record, const char* rawId){
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


bool PluginManager::RegisterAssetImporter(Record& record, const WeAssetImporterDesc& desc){
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


bool PluginManager::UnregisterAssetImporter(Record& record, const char* rawId){
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


void* PluginManager::LookupExport(const std::string& pluginId, const std::string& name, uint32_t minVersion) const{
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


std::vector<PluginManager::PluginScriptFunction> PluginManager::ScriptFunctions() const{
		std::vector<PluginScriptFunction> functions;
		for (const Record& record : m_Records)
		{
			for (const std::string& name : record.RegisteredScriptFunctions)
			{
				World::PluginScriptFunctionInfo info;
				if (!World::PluginScriptLibrary::Describe(record.Entry.Manifest.Id, name, &info))
					continue;
				PluginScriptFunction function;
				function.PluginId = info.PluginId;
				function.Name = info.Name;
				function.Namespace = info.Namespace;
				function.Member = info.Member;
				function.Signature = info.Signature;
				function.Doc = info.Doc;
				functions.push_back(std::move(function));
			}
		}
		return functions;
	}


bool PluginManager::FindScriptFunction(const std::string& pluginId, const std::string& name, PluginScriptFunction* out) const{
		const Record* record = FindRecord(pluginId);
		if (!record || record->Entry.State != PluginState::Loaded)
			return false;
		if (std::find(record->RegisteredScriptFunctions.begin(), record->RegisteredScriptFunctions.end(), name)
			== record->RegisteredScriptFunctions.end())
			return false;
		World::PluginScriptFunctionInfo info;
		if (!World::PluginScriptLibrary::Describe(pluginId, name, &info))
			return false;
		if (out)
		{
			out->PluginId = info.PluginId;
			out->Name = info.Name;
			out->Namespace = info.Namespace;
			out->Member = info.Member;
			out->Signature = info.Signature;
			out->Doc = info.Doc;
		}
		return true;
	}


std::vector<std::shared_ptr<World::Asset::IAssetImporter>> PluginManager::PluginImporters() const{
		std::vector<std::shared_ptr<World::Asset::IAssetImporter>> importers;
		for (const Record& record : m_Records)
			for (const RegisteredImporter& item : record.RegisteredImporters)
				importers.push_back(item.Importer);
		return importers;
	}


bool PluginManager::RegisterComponent(Record& record, World::Schema::SchemaRegistry* registry, const WeComponentDesc& desc){
		const std::string& pluginId = record.Entry.Manifest.Id;

		// 前缀校验:宿主要读 v1 全字段 ⇒ 声明的大小必须覆盖它;ABI 等值门同 WePlugin。
		if (desc.StructSize < sizeof(WeComponentDesc) || desc.AbiVersion != WE_PLUGIN_ABI_VERSION)
		{
			Log(WePluginLogWarn, pluginId + ": component registration rejected (struct/abi mismatch size="
				+ std::to_string(desc.StructSize) + " abi=" + std::to_string(desc.AbiVersion) + ")");
			return false;
		}
		const std::string id = desc.Id ? desc.Id : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": component registration rejected (empty id)");
			return false;
		}
		if (desc.ComponentId != 0)
		{
			// 组件存储 id 由**宿主**分配(T2c 起:声明了 Size 的组件由宿主合成 entt blob
			// 存储并选保留段 id)。插件自报 id 会与宿主分配冲突,所以干净拒绝。
			Log(WePluginLogWarn, pluginId + ": component '" + id + "' registration rejected (ComponentId="
				+ std::to_string(desc.ComponentId)
				+ "; no storage bridge is owned by plugins - the host assigns component storage ids,"
				" plugins must pass ComponentId=0)");
			return false;
		}
		if (desc.FieldCount > 0 && !desc.Fields)
		{
			Log(WePluginLogWarn, pluginId + ": component '" + id
				+ "' registration rejected (field count > 0 but the table is null)");
			return false;
		}
		if (desc.FieldCount > kMaxComponentFieldSlots)
		{
			Log(WePluginLogWarn, pluginId + ": component '" + id + "' registration rejected ("
				+ std::to_string(desc.FieldCount) + " fields exceed the host slot table of "
				+ std::to_string(kMaxComponentFieldSlots) + ")");
			return false;
		}
		for (const RegisteredComponent& item : record.RegisteredComponents)
		{
			if (item.Id == id)
			{
				Log(WePluginLogWarn, pluginId + ": component '" + id
					+ "' is already registered by this plugin; registration ignored");
				return false;
			}
		}

		if (!registry)
		{
			Log(WePluginLogWarn, pluginId + ": component '" + id
				+ "' registration rejected (no schema registry handle for this plugin)");
			return false;
		}

		// ---- T2c:存储桥声明(Size / Alignment)校验 --------------------------------
		// Size == 0 = 保持 T2b 的 schema-only 行为(两字段都必须为 0);Size > 0 = 宿主为它
		// 合成一个固定尺寸 blob 存储,组件能挂到场景实体上(Add/Remove/Copy/序列化/属性面板
		// 全走既有 schema 通路)。
		const uint32_t declaredSize = desc.Size;
		const uint32_t declaredAlignment = desc.Alignment;
		if (declaredSize == 0 && declaredAlignment != 0)
		{
			Log(WePluginLogWarn, pluginId + ": component '" + id
				+ "' registration rejected (alignment " + std::to_string(declaredAlignment)
				+ " declared without a size; schema-only components must pass Size=0 and Alignment=0)");
			return false;
		}
		if (declaredSize > kPluginComponentMaxBytes)
		{
			Log(WePluginLogWarn, pluginId + ": component '" + id + "' registration rejected (declared size "
				+ std::to_string(declaredSize) + " exceeds the host blob maximum of "
				+ std::to_string(kPluginComponentMaxBytes) + " bytes)");
			return false;
		}
		if (declaredSize > 0 && declaredAlignment != 0)
		{
			const bool powerOfTwo = (declaredAlignment & (declaredAlignment - 1u)) == 0u;
			if (!powerOfTwo || declaredAlignment > kPluginComponentAlignmentCap)
			{
				Log(WePluginLogWarn, pluginId + ": component '" + id + "' registration rejected (declared alignment "
					+ std::to_string(declaredAlignment) + " must be a power of two <= "
					+ std::to_string(kPluginComponentAlignmentCap) + ")");
				return false;
			}
			if (declaredSize % declaredAlignment != 0)
			{
				Log(WePluginLogWarn, pluginId + ": component '" + id + "' registration rejected (declared size "
					+ std::to_string(declaredSize) + " is not a multiple of the declared alignment "
					+ std::to_string(declaredAlignment) + ")");
				return false;
			}
		}

		World::Schema::TypeSchema schema;
		schema.Id = World::Schema::TypeId(id);
		// PLUG-CLEAN-1:显示名按约定键 `plugin.<pluginId>.<typeId>` 本地化(见
		// ResolveComponentDisplayName;缺条目 = 插件声明的 DisplayName,行为不变)。
		schema.DisplayName = ResolveComponentDisplayName(pluginId, id,
			desc.DisplayName && desc.DisplayName[0] ? desc.DisplayName : "");
		schema.Category = World::Schema::TypeCategory::Component;
		// T2c:Size = 插件声明的结构总大小(0 = T2b 的 schema-only);Storage 在下面按声明
		// 合成本次注册专属的 blob 存储绑定(只在注册进注册表**之前**填,保证指针稳定)。
		schema.Size = declaredSize;
		schema.Storage = nullptr;

		std::vector<uint32_t> slots;
		std::set<std::string> fieldNames;
		std::string failure;
		for (uint32_t index = 0; index < desc.FieldCount; ++index)
		{
			const WeComponentFieldDesc& field = desc.Fields[index];
			if (field.StructSize < sizeof(WeComponentFieldDesc)
				|| field.AbiVersion != WE_PLUGIN_ABI_VERSION)
			{
				failure = "field " + std::to_string(index) + " has struct/abi mismatch (size="
					+ std::to_string(field.StructSize) + " abi=" + std::to_string(field.AbiVersion) + ")";
				break;
			}
			const std::string fieldName = field.Name ? field.Name : "";
			if (fieldName.empty())
			{
				failure = "field " + std::to_string(index) + " has an empty name";
				break;
			}
			if (!fieldNames.insert(fieldName).second)
			{
				failure = "duplicate field name '" + fieldName + "'";
				break;
			}
			const uint32_t canonicalSize = ComponentFieldKindSize(field.Kind);
			if (canonicalSize == 0)
			{
				failure = "field '" + fieldName + "' has unsupported kind "
					+ std::to_string(field.Kind)
					+ " (T2b supports fixed-size POD kinds only)";
				break;
			}
			if (field.Size != canonicalSize)
			{
				failure = "field '" + fieldName + "' size " + std::to_string(field.Size)
					+ " does not match its kind (expected " + std::to_string(canonicalSize) + ")";
				break;
			}
			// T2c:声明了结构大小时,每个字段必须落在结构内 —— 否则 blob 存储会越界。
			if (declaredSize > 0 && (field.Offset > declaredSize
				|| canonicalSize > declaredSize - field.Offset))
			{
				failure = "field '" + fieldName + "' (offset " + std::to_string(field.Offset)
					+ " + size " + std::to_string(canonicalSize)
					+ ") extends past the declared component size " + std::to_string(declaredSize);
				break;
			}
			const uint32_t slot = AllocateComponentFieldSlot(field.Kind, field.Offset);
			if (slot >= kMaxComponentFieldSlots)
			{
				failure = "field accessor slot table is exhausted";
				break;
			}
			slots.push_back(slot);

			World::Schema::FieldSchema converted;
			converted.Id = World::Schema::FieldId { World::Schema::Fnv1a64(fieldName) };
			converted.Name = fieldName;
			converted.K = static_cast<World::Schema::Kind>(field.Kind);
			converted.Get = kComponentFieldGetters[slot];
			converted.Set = kComponentFieldSetters[slot];
			converted.Meta.Doc = field.Doc ? field.Doc : "";
			converted.Meta.Color = (field.Flags & WeComponentFieldFlagColor) != 0;
			converted.Meta.ReadOnly = (field.Flags & WeComponentFieldFlagReadOnly) != 0;
			for (uint32_t choice = 0; choice < field.ChoicesCount && field.Choices; ++choice)
				if (field.Choices[choice] && field.Choices[choice][0])
					converted.Meta.Choices.emplace_back(field.Choices[choice]);
			schema.Fields.push_back(std::move(converted));
		}
		if (!failure.empty())
		{
			for (uint32_t slot : slots)
				ReleaseComponentFieldSlot(slot);
			Log(WePluginLogWarn, pluginId + ": component '" + id + "' registration rejected (" + failure + ")");
			return false;
		}

		// 存储绑定:在册槽位 + blob 类型(档位由声明大小决定)。绑定先建好再注册 ——
		// 注册表里的 TypeSchema 拷贝持有它的地址,而 Schema.Storage 必须指向稳定地址。
		RegisteredComponent entry;
		entry.Id = id;
		entry.Schema = std::move(schema);
		entry.Slots = std::move(slots);
		if (declaredSize > 0)
		{
			const uint32_t componentSlot = AllocateComponentSlot();
			if (componentSlot >= kPluginComponentSlotCount)
			{
				for (uint32_t slot : entry.Slots)
					ReleaseComponentFieldSlot(slot);
				Log(WePluginLogWarn, pluginId + ": component '" + id
					+ "' registration rejected (plugin component slot table is exhausted, max "
					+ std::to_string(kPluginComponentSlotCount) + ")");
				return false;
			}
			entry.Storage = std::make_unique<World::Schema::StorageBinding>();
			std::string storageError;
			if (!MakePluginComponentStorageBinding(declaredSize, declaredAlignment, componentSlot,
				entry.Storage.get(), &storageError))
			{
				ReleaseComponentSlot(componentSlot);
				for (uint32_t slot : entry.Slots)
					ReleaseComponentFieldSlot(slot);
				Log(WePluginLogWarn, pluginId + ": component '" + id + "' registration rejected ("
					+ storageError + ")");
				return false;
			}
			entry.Schema.Storage = entry.Storage.get();
			entry.ComponentSlot = componentSlot;
			entry.DeclaredSize = declaredSize;
		}

		// 事务化:注册表校验失败不提交任何条目;失败时释放本次分配的槽位(不半注册)。
		const World::Schema::SchemaRegistry::Status status = registry->RegisterModule(
			PluginSchemaModule(pluginId), std::vector<World::Schema::TypeSchema> { entry.Schema });
		if (status != World::Schema::SchemaRegistry::Status::Ok)
		{
			if (entry.Storage)
				ReleaseComponentSlot(entry.ComponentSlot);
			for (uint32_t slot : entry.Slots)
				ReleaseComponentFieldSlot(slot);
			Log(WePluginLogWarn, pluginId + ": component '" + id + "' registration rejected by the schema registry ("
				+ World::Schema::SchemaRegistry::StatusName(status) + ")");
			return false;
		}

		const uint32_t storageId = entry.Schema.Storage ? entry.Schema.Storage->ComponentId : 0;
		const uint32_t fieldCount = static_cast<uint32_t>(entry.Schema.Fields.size());
		record.RegisteredComponents.push_back(std::move(entry));
		Log(WePluginLogInfo, pluginId + ": registered component '" + id + "' ("
			+ std::to_string(fieldCount) + " field(s)"
			+ (declaredSize > 0 ? ", " + std::to_string(declaredSize) + "-byte blob storage id="
				+ Hex32(storageId) : std::string())
			+ ")");
		return true;
	}


bool PluginManager::UnregisterComponent(Record& record, World::Schema::SchemaRegistry* registry, const char* rawId, WorldContext* context){
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string id = rawId ? rawId : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": component unregister rejected (empty id)");
			return false;
		}

		const auto tracked = std::find_if(record.RegisteredComponents.begin(),
			record.RegisteredComponents.end(),
			[&id](const RegisteredComponent& item) { return item.Id == id; });
		if (tracked == record.RegisteredComponents.end())
		{
			// 幂等口径:本插件没注册过 → true 且不报错;但别人的类型必须拒绝(不能顺手删掉)。
			if (registry && registry->Find(id))
			{
				Log(WePluginLogWarn, pluginId + ": component '" + id
					+ "' is not owned by this plugin; unregister ignored");
				return false;
			}
			return true;
		}
		if (!registry)
		{
			Log(WePluginLogWarn, pluginId + ": component '" + id
				+ "' unregister rejected (no schema registry handle for this plugin)");
			return false;
		}

		// ---- T6:活实例门(与 Unload / UnloadForReload 对称)----------------------------
		// 注销类型会让 blob 实例变成"没有 schema 的孤儿"(序列化直接丢数据),所以有实例时
		// 干净拒绝:先移除组件/销毁场景,再注销。schema-only 组件(没有存储)不受影响。
		if (tracked->Storage)
		{
			const std::size_t instances = World::Scene::CountLiveComponentInstances(
				tracked->Storage->ComponentId, context);
			if (instances > 0)
			{
				Log(WePluginLogWarn, pluginId + ": component '" + id + "' unregister refused ("
					+ std::to_string(instances) + " live component instance(s) use its storage id 0x"
					+ Hex32(tracked->Storage->ComponentId)
					+ "; remove the components or destroy the scene first)");
				return false;
			}
		}

		// SchemaRegistry 只有模块级注销 ⇒ 把该插件的其余类型按原顺序重新注册回同一模块。
		// 先从账本取拷贝(UnregisterModule 会销毁注册表里的原条目),再整体注销 + 重注册。
		std::vector<World::Schema::TypeSchema> remaining;
		remaining.reserve(record.RegisteredComponents.size() - 1);
		for (const RegisteredComponent& item : record.RegisteredComponents)
			if (item.Id != id)
				remaining.push_back(item.Schema);

		registry->UnregisterModule(PluginSchemaModule(pluginId));
		if (!remaining.empty())
		{
			const World::Schema::SchemaRegistry::Status status = registry->RegisterModule(
				PluginSchemaModule(pluginId), remaining);
			if (status != World::Schema::SchemaRegistry::Status::Ok)
			{
				// 其余类型此前都通过过校验,这里理论不可达;真发生 = 整个模块被丢弃并记 ERROR,
				// 账本与槽位保持"模块为空"的一致状态(不留悬空访问器,也不留脏账)。
				Log(WePluginLogError, pluginId + ": re-registering the remaining component types failed ("
					+ World::Schema::SchemaRegistry::StatusName(status)
					+ "); the plugin's component module was dropped");
				for (const RegisteredComponent& item : record.RegisteredComponents)
				{
					for (uint32_t slot : item.Slots)
						ReleaseComponentFieldSlot(slot);
					if (item.Storage)
						ReleaseComponentSlot(item.ComponentSlot);
				}
				record.RegisteredComponents.clear();
				return false;
			}
		}

		for (uint32_t slot : tracked->Slots)
			ReleaseComponentFieldSlot(slot);
		const bool hadStorage = tracked->Storage != nullptr;
		const uint32_t componentSlot = tracked->ComponentSlot;
		record.RegisteredComponents.erase(tracked);
		if (hadStorage)
			ReleaseComponentSlot(componentSlot);
		Log(WePluginLogInfo, pluginId + ": unregistered component '" + id + "'");
		return true;
	}


void PluginManager::ReclaimComponentTypes(Record& record, World::Schema::SchemaRegistry* schemas){
		if (record.RegisteredComponents.empty())
			return;
		const std::string& pluginId = record.Entry.Manifest.Id;
		for (const RegisteredComponent& item : record.RegisteredComponents)
			Log(WePluginLogWarn, pluginId + ": component '" + item.Id
				+ "' was not unregistered by the plugin; force-removed");
		if (schemas)
			schemas->UnregisterModule(PluginSchemaModule(pluginId));
		else
			Log(WePluginLogError, pluginId
				+ ": component schema module could not be removed (no schema registry handle)");
		for (const RegisteredComponent& item : record.RegisteredComponents)
		{
			for (uint32_t slot : item.Slots)
				ReleaseComponentFieldSlot(slot);
			if (item.Storage)
				ReleaseComponentSlot(item.ComponentSlot);
		}
		record.RegisteredComponents.clear();
	}

}
