#include "PluginManager_Internal.h"

namespace World::Plugins
{

using namespace PluginManagerDetail;

namespace PluginManagerDetail
{

uint32_t AllocateComponentFieldSlot(uint32_t kind, uint32_t offset){
			for (uint32_t slot = 0; slot < kMaxComponentFieldSlots; ++slot)
			{
				if (g_ComponentFieldSlots[slot].InUse)
					continue;
				g_ComponentFieldSlots[slot].InUse = true;
				g_ComponentFieldSlots[slot].Kind = kind;
				g_ComponentFieldSlots[slot].Offset = offset;
				return slot;
			}
			return kMaxComponentFieldSlots;   // 用尽 = 本次注册失败(可读诊断)
		}


void ReleaseComponentFieldSlot(uint32_t slot){
			if (slot < kMaxComponentFieldSlots)
				g_ComponentFieldSlots[slot] = ComponentFieldSlot {};
		}


		// 字段 Kind 的规范字节数(0 = T2b 不支持该 Kind;String/Enum/Asset/Object 与容器不支持)。
uint32_t ComponentFieldKindSize(uint32_t kind){
			switch (static_cast<WeComponentFieldKind>(kind))
			{
				case WeComponentKindBool: return sizeof(bool);
				case WeComponentKindInt8: return sizeof(int8_t);
				case WeComponentKindInt16: return sizeof(int16_t);
				case WeComponentKindInt32: return sizeof(int32_t);
				case WeComponentKindInt64: return sizeof(int64_t);
				case WeComponentKindUInt8: return sizeof(uint8_t);
				case WeComponentKindUInt16: return sizeof(uint16_t);
				case WeComponentKindUInt32: return sizeof(uint32_t);
				case WeComponentKindUInt64: return sizeof(uint64_t);
				case WeComponentKindFloat: return sizeof(float);
				case WeComponentKindDouble: return sizeof(double);
				case WeComponentKindVec2: return sizeof(glm::vec2);
				case WeComponentKindVec3: return sizeof(glm::vec3);
				case WeComponentKindVec4: return sizeof(glm::vec4);
				case WeComponentKindIVec2: return sizeof(glm::ivec2);
				case WeComponentKindIVec3: return sizeof(glm::ivec3);
				case WeComponentKindIVec4: return sizeof(glm::ivec4);
				case WeComponentKindUVec2: return sizeof(glm::uvec2);
				case WeComponentKindUVec3: return sizeof(glm::uvec3);
				case WeComponentKindUVec4: return sizeof(glm::uvec4);
				case WeComponentKindQuat: return sizeof(glm::quat);
				case WeComponentKindMat3: return sizeof(glm::mat3);
				case WeComponentKindMat4: return sizeof(glm::mat4);
				default: return 0;
			}
		}


bool ReadComponentFieldValue(uint32_t kind, const uint8_t* address, World::Schema::Value* out){
			switch (static_cast<WeComponentFieldKind>(kind))
			{
				case WeComponentKindBool: return ReadPodValue<bool>(address, out);
				case WeComponentKindInt8: return ReadPodValue<int8_t>(address, out);
				case WeComponentKindInt16: return ReadPodValue<int16_t>(address, out);
				case WeComponentKindInt32: return ReadPodValue<int32_t>(address, out);
				case WeComponentKindInt64: return ReadPodValue<int64_t>(address, out);
				case WeComponentKindUInt8: return ReadPodValue<uint8_t>(address, out);
				case WeComponentKindUInt16: return ReadPodValue<uint16_t>(address, out);
				case WeComponentKindUInt32: return ReadPodValue<uint32_t>(address, out);
				case WeComponentKindUInt64: return ReadPodValue<uint64_t>(address, out);
				case WeComponentKindFloat: return ReadPodValue<float>(address, out);
				case WeComponentKindDouble: return ReadPodValue<double>(address, out);
				case WeComponentKindVec2: return ReadPodValue<glm::vec2>(address, out);
				case WeComponentKindVec3: return ReadPodValue<glm::vec3>(address, out);
				case WeComponentKindVec4: return ReadPodValue<glm::vec4>(address, out);
				case WeComponentKindIVec2: return ReadPodValue<glm::ivec2>(address, out);
				case WeComponentKindIVec3: return ReadPodValue<glm::ivec3>(address, out);
				case WeComponentKindIVec4: return ReadPodValue<glm::ivec4>(address, out);
				case WeComponentKindUVec2: return ReadPodValue<glm::uvec2>(address, out);
				case WeComponentKindUVec3: return ReadPodValue<glm::uvec3>(address, out);
				case WeComponentKindUVec4: return ReadPodValue<glm::uvec4>(address, out);
				case WeComponentKindQuat: return ReadPodValue<glm::quat>(address, out);
				case WeComponentKindMat3: return ReadPodValue<glm::mat3>(address, out);
				case WeComponentKindMat4: return ReadPodValue<glm::mat4>(address, out);
				default: return false;
			}
		}


bool WriteComponentFieldValue(uint32_t kind, uint8_t* address, const World::Schema::Value& value){
			switch (static_cast<WeComponentFieldKind>(kind))
			{
				case WeComponentKindBool: return WritePodValue<bool>(address, value);
				case WeComponentKindInt8: return WritePodValue<int8_t>(address, value);
				case WeComponentKindInt16: return WritePodValue<int16_t>(address, value);
				case WeComponentKindInt32: return WritePodValue<int32_t>(address, value);
				case WeComponentKindInt64: return WritePodValue<int64_t>(address, value);
				case WeComponentKindUInt8: return WritePodValue<uint8_t>(address, value);
				case WeComponentKindUInt16: return WritePodValue<uint16_t>(address, value);
				case WeComponentKindUInt32: return WritePodValue<uint32_t>(address, value);
				case WeComponentKindUInt64: return WritePodValue<uint64_t>(address, value);
				case WeComponentKindFloat: return WritePodValue<float>(address, value);
				case WeComponentKindDouble: return WritePodValue<double>(address, value);
				case WeComponentKindVec2: return WritePodValue<glm::vec2>(address, value);
				case WeComponentKindVec3: return WritePodValue<glm::vec3>(address, value);
				case WeComponentKindVec4: return WritePodValue<glm::vec4>(address, value);
				case WeComponentKindIVec2: return WritePodValue<glm::ivec2>(address, value);
				case WeComponentKindIVec3: return WritePodValue<glm::ivec3>(address, value);
				case WeComponentKindIVec4: return WritePodValue<glm::ivec4>(address, value);
				case WeComponentKindUVec2: return WritePodValue<glm::uvec2>(address, value);
				case WeComponentKindUVec3: return WritePodValue<glm::uvec3>(address, value);
				case WeComponentKindUVec4: return WritePodValue<glm::uvec4>(address, value);
				case WeComponentKindQuat: return WritePodValue<glm::quat>(address, value);
				case WeComponentKindMat3: return WritePodValue<glm::mat3>(address, value);
				case WeComponentKindMat4: return WritePodValue<glm::mat4>(address, value);
				default: return false;
			}
		}


		// 组件 schema 的模块归属 = 插件 id(SchemaRegistry 的 UnregisterModule/ListByModule
		// 只按 Name 比较;Version 仅存档)。
World::Schema::ModuleId PluginSchemaModule(const std::string& pluginId){
			World::Schema::ModuleId module;
			module.Name = pluginId;
			module.Version = 1;
			return module;
		}

}

const char* PluginManager::StatusName(Status status){
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
			case Status::HasLiveInstances: return "plugin component still has live instances";
			case Status::NotSafePoint: return "not at a plugin reload safe point";
		}
		return "unknown";
	}


PluginManager::PluginManager(){
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
		// T2b 组件 schema 注册面(同上:尾部追加)。
		m_HostApi.RegisterComponent = &PluginManager::BridgeRegisterComponent;
		m_HostApi.UnregisterComponent = &PluginManager::BridgeUnregisterComponent;
		// T3b 编辑器扩展面(同上:尾部追加)。
		m_HostApi.RegisterEditorCommand = &PluginManager::BridgeRegisterEditorCommand;
		m_HostApi.UnregisterEditorCommand = &PluginManager::BridgeUnregisterEditorCommand;
		m_HostApi.RegisterEditorPanel = &PluginManager::BridgeRegisterEditorPanel;
		m_HostApi.UnregisterEditorPanel = &PluginManager::BridgeUnregisterEditorPanel;
		// T4 脚本函数库(同上:尾部追加)。
		m_HostApi.RegisterScriptFunction = &PluginManager::BridgeRegisterScriptFunction;
		m_HostApi.UnregisterScriptFunction = &PluginManager::BridgeUnregisterScriptFunction;
	}


PluginManager::~PluginManager(){
		if (LoadedCount() > 0)
			Log(WePluginLogError, "manager destroyed with " + std::to_string(LoadedCount())
				+ " plugin(s) still loaded (host must call UnloadAll first)");
		// T6:未完成第二段的实例快照只在内存里 —— 管理器析构 = 这些实例数据丢失。
		for (const auto& [id, pending] : m_PendingReloads)
			Log(WePluginLogError, "pending reload state for plugin '" + id + "' with "
				+ std::to_string(pending.InstancesSnapshotted)
				+ " snapshotted component instance(s) was dropped (host must finish reload before shutdown)");
		m_PendingReloads.clear();
		// T2:管理器析构会让 Library 一起释放。即使宿主违约(没先 UnloadAll),也必须把
		// 指向这些 DLL 的注册回调从宿主注册面移除 —— 否则留下悬空回调。
		// T2b:组件 schema 用 HostApiBox 记住的注册表句柄整模块注销(宿主必须在 WorldContext
		// 存活期间销毁本管理器 —— 这是"先 UnloadAll 再析构"契约的一部分)。
		for (Record& record : m_Records)
			ReclaimPluginRegistrations(record, record.Host ? record.Host->Schemas : nullptr);
	}


void PluginManager::Log(int level, const std::string& text){
		// 统一 `[plugin]` 前缀;用 "{0}" 承载正文,避免正文里的花括号被当成 spdlog 格式串。
		const std::string line = "[plugin] " + text;
		switch (level)
		{
			case WePluginLogWarn: WLD_CORE_WARN("{0}", line); break;
			case WePluginLogError: WLD_CORE_ERROR("{0}", line); break;
			default: WLD_CORE_INFO("{0}", line); break;
		}
	}


void PluginManager::LogBridge(void* userData, int level, const char* message){
		// UserData = 本次加载的 HostApiBox(插件只原样回传);前缀里带上插件 id。
		const auto* box = static_cast<const HostApiBox*>(userData);
		if (!box)
			return;
		Log(level, box->PluginId + ": " + (message ? message : ""));
	}


std::vector<PluginEntry> PluginManager::Entries() const{
		std::vector<PluginEntry> entries;
		entries.reserve(m_Records.size());
		for (const Record& record : m_Records)
			entries.push_back(record.Entry);
		return entries;
	}


size_t PluginManager::LoadedCount() const{
		size_t count = 0;
		for (const Record& record : m_Records)
			if (record.Entry.State == PluginState::Loaded)
				++count;
		return count;
	}


std::vector<std::string> PluginManager::LoadOrder() const{
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


PluginManager::Record* PluginManager::FindRecord(const std::string& id){
		for (Record& record : m_Records)
			if (record.Entry.Manifest.Id == id)
				return &record;
		return nullptr;
	}


const PluginManager::Record* PluginManager::FindRecord(const std::string& id) const{
		for (const Record& record : m_Records)
			if (record.Entry.Manifest.Id == id)
				return &record;
		return nullptr;
	}


const PluginEntry* PluginManager::Find(const std::string& id) const{
		const Record* record = FindRecord(id);
		return record ? &record->Entry : nullptr;
	}

}
