#include "wldpch.h"
#include "World/Plugins/PluginManager.h"
#include "World/Core/Asset/AssetTypeRegistry.h"
#include "World/Core/WorldContext.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Scene/Scene.h"
#include "World/Script/PluginScriptLibrary.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace World::Plugins
{
	namespace
	{
		// 诊断用:插件组件存储 id(保留段)写成 0xXXXXXXXX。
		std::string Hex32(uint32_t value)
		{
			char buffer[16] = {};
			std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
			return buffer;
		}

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

		// PLUG-CLEAN-1:插件组件显示名的本地化约定(契约见 docs/dev/plugin-framework.md):
		//   键 = "plugin.<pluginId>.<typeId>";先按类型全名(WeComponentDesc::Id)查,
		//   未命中再按短名(最后一个 '.' 之后)查一次;都没命中 ⇒ 插件声明的 DisplayName。
		// 解析发生在**加载/重载时**(与组件注册同一生命周期):schema.DisplayName 同时是
		// 编辑期显示文案与 a11y 行 id(`prop.add.<DisplayName>`)的来源,不能每帧改写;
		// 切换语言后由 plugin.reload 或重启编辑器重新解析。
		std::string ResolveComponentDisplayName(const std::string& pluginId, const std::string& typeId,
			const std::string& declared)
		{
			const std::string fallback = declared.empty() ? typeId : declared;
			const Wui::LocalizedLabel full = Wui::TrLabel("plugin." + pluginId + "." + typeId, fallback);
			if (full.Text != fallback)
				return full.Text;
			const size_t separator = typeId.rfind('.');
			if (separator != std::string::npos && separator + 1 < typeId.size())
			{
				const std::string shortName = typeId.substr(separator + 1);
				const Wui::LocalizedLabel shortLabel =
					Wui::TrLabel("plugin." + pluginId + "." + shortName, fallback);
				if (shortLabel.Text != fallback)
					return shortLabel.Text;
			}
			return fallback;
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

		// ---- T2b:组件 schema 注册面 → SchemaRegistry 的适配 --------------------------
		//
		// 组件注册 = 纯 schema 注册(TypeCategory::Component + Storage == nullptr):
		// 类型进入 SchemaRegistry(Find/List/ListByModule 与序列化 API 可见),但**不参与
		// entt 实例化** —— 存储桥(Add/Copy 回调)不在 T2b 的 ABI 里,宿主不伪造 Storage
		// (伪造会让 Add Component 选择器列出无法实例化的组件)。
		//
		// 字段读写 = 宿主侧访问器按 (Kind, Offset) 从实例内存读出/写入 Value:插件拥有组件
		// 布局,宿主只校验 Kind 与 Size 的对应关系;实例指针由消费方(插件自己的存储/会话/测试)
		// 提供。Schema::FieldSchema 的 Get/Set 是不带 userData 的裸函数指针,而 (Kind, Offset)
		// 是运行期数据 ⇒ 用固定槽位表把状态绑到一对 template<size_t> 函数上;槽位随注册分配、
		// 随注销/兜底回收释放。注册面按 ABI 契约在插件 Register/Unregister 调用期间使用(单线程)。
		constexpr uint32_t kMaxComponentFieldSlots = 256;

		struct ComponentFieldSlot
		{
			bool InUse = false;
			uint32_t Kind = WeComponentKindNone;
			uint32_t Offset = 0;
		};

		ComponentFieldSlot g_ComponentFieldSlots[kMaxComponentFieldSlots];

		uint32_t AllocateComponentFieldSlot(uint32_t kind, uint32_t offset)
		{
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

		void ReleaseComponentFieldSlot(uint32_t slot)
		{
			if (slot < kMaxComponentFieldSlots)
				g_ComponentFieldSlots[slot] = ComponentFieldSlot {};
		}

		// 字段 Kind 的规范字节数(0 = T2b 不支持该 Kind;String/Enum/Asset/Object 与容器不支持)。
		uint32_t ComponentFieldKindSize(uint32_t kind)
		{
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

		template <typename T>
		bool ReadPodValue(const uint8_t* address, World::Schema::Value* out)
		{
			static_assert(std::is_trivially_copyable<T>::value, "组件字段必须是可平凡复制的 POD");
			T value {};
			std::memcpy(&value, address, sizeof(T));
			*out = World::Schema::Value(value);
			return true;
		}

		template <typename T>
		bool WritePodValue(uint8_t* address, const World::Schema::Value& value)
		{
			static_assert(std::is_trivially_copyable<T>::value, "组件字段必须是可平凡复制的 POD");
			const T* typed = std::get_if<T>(&value);
			if (!typed)
				return false;   // 类型不符 = 不写(与"未设不覆盖"同口径)
			std::memcpy(address, typed, sizeof(T));
			return true;
		}

		bool ReadComponentFieldValue(uint32_t kind, const uint8_t* address, World::Schema::Value* out)
		{
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

		bool WriteComponentFieldValue(uint32_t kind, uint8_t* address, const World::Schema::Value& value)
		{
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

		// 槽位化的字段访问器:状态在 g_ComponentFieldSlots[Slot](见上)。
		template <size_t Slot>
		World::Schema::Value ComponentFieldSlotGet(const void* instance)
		{
			World::Schema::Value value;
			const ComponentFieldSlot& state = g_ComponentFieldSlots[Slot];
			if (!instance || !state.InUse)
				return value;
			ReadComponentFieldValue(state.Kind,
				static_cast<const uint8_t*>(instance) + state.Offset, &value);
			return value;
		}

		template <size_t Slot>
		void ComponentFieldSlotSet(void* instance, const World::Schema::Value& value)
		{
			const ComponentFieldSlot& state = g_ComponentFieldSlots[Slot];
			if (!instance || !state.InUse)
				return;
			WriteComponentFieldValue(state.Kind,
				static_cast<uint8_t*>(instance) + state.Offset, value);
		}

		template <size_t... Indices>
		constexpr std::array<World::Schema::Value (*)(const void*), sizeof...(Indices)>
			MakeComponentFieldGetters(std::index_sequence<Indices...>)
		{
			return { &ComponentFieldSlotGet<Indices>... };
		}

		template <size_t... Indices>
		constexpr std::array<void (*)(void*, const World::Schema::Value&), sizeof...(Indices)>
			MakeComponentFieldSetters(std::index_sequence<Indices...>)
		{
			return { &ComponentFieldSlotSet<Indices>... };
		}

		constexpr auto kComponentFieldGetters =
			MakeComponentFieldGetters(std::make_index_sequence<kMaxComponentFieldSlots>{});
		constexpr auto kComponentFieldSetters =
			MakeComponentFieldSetters(std::make_index_sequence<kMaxComponentFieldSlots>{});

		// 组件 schema 的模块归属 = 插件 id(SchemaRegistry 的 UnregisterModule/ListByModule
		// 只按 Name 比较;Version 仅存档)。
		World::Schema::ModuleId PluginSchemaModule(const std::string& pluginId)
		{
			World::Schema::ModuleId module;
			module.Name = pluginId;
			module.Version = 1;
			return module;
		}

		// T2b 公共契约:WeComponentFieldKind 的数值 = World::Schema::Kind 的稳定值。
		// 逐项钉住(改这里 = 改 ABI,必须走 WePluginApi.h 的升版流程)。
#define WE_T2B_KIND_ASSERT(abiName, schemaName) \
		static_assert(static_cast<uint32_t>(World::Schema::Kind::schemaName) == abiName, \
			"WeComponentFieldKind must mirror World::Schema::Kind values")
		WE_T2B_KIND_ASSERT(WeComponentKindNone, None);
		WE_T2B_KIND_ASSERT(WeComponentKindBool, Bool);
		WE_T2B_KIND_ASSERT(WeComponentKindInt8, Int8);
		WE_T2B_KIND_ASSERT(WeComponentKindInt16, Int16);
		WE_T2B_KIND_ASSERT(WeComponentKindInt32, Int32);
		WE_T2B_KIND_ASSERT(WeComponentKindInt64, Int64);
		WE_T2B_KIND_ASSERT(WeComponentKindUInt8, UInt8);
		WE_T2B_KIND_ASSERT(WeComponentKindUInt16, UInt16);
		WE_T2B_KIND_ASSERT(WeComponentKindUInt32, UInt32);
		WE_T2B_KIND_ASSERT(WeComponentKindUInt64, UInt64);
		WE_T2B_KIND_ASSERT(WeComponentKindFloat, Float);
		WE_T2B_KIND_ASSERT(WeComponentKindDouble, Double);
		WE_T2B_KIND_ASSERT(WeComponentKindVec2, Vec2);
		WE_T2B_KIND_ASSERT(WeComponentKindVec3, Vec3);
		WE_T2B_KIND_ASSERT(WeComponentKindVec4, Vec4);
		WE_T2B_KIND_ASSERT(WeComponentKindIVec2, IVec2);
		WE_T2B_KIND_ASSERT(WeComponentKindIVec3, IVec3);
		WE_T2B_KIND_ASSERT(WeComponentKindIVec4, IVec4);
		WE_T2B_KIND_ASSERT(WeComponentKindUVec2, UVec2);
		WE_T2B_KIND_ASSERT(WeComponentKindUVec3, UVec3);
		WE_T2B_KIND_ASSERT(WeComponentKindUVec4, UVec4);
		WE_T2B_KIND_ASSERT(WeComponentKindQuat, Quat);
		WE_T2B_KIND_ASSERT(WeComponentKindMat3, Mat3);
		WE_T2B_KIND_ASSERT(WeComponentKindMat4, Mat4);
		WE_T2B_KIND_ASSERT(WeComponentKindString, String);
		WE_T2B_KIND_ASSERT(WeComponentKindEnum, Enum);
		WE_T2B_KIND_ASSERT(WeComponentKindAsset, Asset);
		WE_T2B_KIND_ASSERT(WeComponentKindObject, Object);
#undef WE_T2B_KIND_ASSERT
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
			case Status::HasLiveInstances: return "plugin component still has live instances";
			case Status::NotSafePoint: return "not at a plugin reload safe point";
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

	PluginManager::~PluginManager()
	{
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

	bool PluginManager::BridgeRegisterComponent(void* userData, const WeComponentDesc* desc)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": component registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterComponent(*record, box->Schemas, *desc);
	}

	bool PluginManager::BridgeUnregisterComponent(void* userData, const char* id)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": component unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterComponent(*record, box->Schemas, id, box->Context);
	}

	// ---- T3b:编辑器扩展(命令 / 面板)桥 ------------------------------------------------

	bool PluginManager::BridgeRegisterEditorCommand(void* userData, const WeEditorCommandDesc* desc)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": editor command registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterEditorCommand(*record, *desc);
	}

	bool PluginManager::BridgeUnregisterEditorCommand(void* userData, const char* id)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": editor command unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterEditorCommand(*record, id);
	}

	bool PluginManager::BridgeRegisterEditorPanel(void* userData, const WeEditorPanelDesc* desc)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record || !desc)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": editor panel registration rejected ("
				+ (!desc ? "null descriptor" : "invalid host handle") + ")");
			return false;
		}
		return box->Manager->RegisterEditorPanel(*record, *desc);
	}

	bool PluginManager::BridgeUnregisterEditorPanel(void* userData, const char* id)
	{
		auto* box = static_cast<HostApiBox*>(userData);
		if (!box || !box->Manager)
			return false;
		Record* record = box->Manager->ResolveHostRecord(*box);
		if (!record)
		{
			Log(WePluginLogWarn, (box->PluginId.empty() ? std::string("unknown plugin") : box->PluginId)
				+ ": editor panel unregister rejected (invalid host handle)");
			return false;
		}
		return box->Manager->UnregisterEditorPanel(*record, id);
	}

	// ---- T4:脚本函数库桥 ----------------------------------------------------------------

	bool PluginManager::BridgeRegisterScriptFunction(void* userData, const WeScriptFunctionDesc* desc)
	{
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

	bool PluginManager::BridgeUnregisterScriptFunction(void* userData, const char* name)
	{
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

	std::vector<PluginManager::PluginScriptFunction> PluginManager::ScriptFunctions() const
	{
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

	bool PluginManager::FindScriptFunction(const std::string& pluginId, const std::string& name,
		PluginScriptFunction* out) const
	{
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

	std::vector<std::shared_ptr<World::Asset::IAssetImporter>> PluginManager::PluginImporters() const
	{
		std::vector<std::shared_ptr<World::Asset::IAssetImporter>> importers;
		for (const Record& record : m_Records)
			for (const RegisteredImporter& item : record.RegisteredImporters)
				importers.push_back(item.Importer);
		return importers;
	}

	bool PluginManager::RegisterComponent(Record& record, World::Schema::SchemaRegistry* registry,
		const WeComponentDesc& desc)
	{
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

	bool PluginManager::UnregisterComponent(Record& record, World::Schema::SchemaRegistry* registry,
		const char* rawId, WorldContext* context)
	{
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

	void PluginManager::ReclaimComponentTypes(Record& record, World::Schema::SchemaRegistry* schemas)
	{
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

	// ---- T2c:插件组件存储的在册槽位 ------------------------------------------------

	uint32_t PluginManager::AllocateComponentSlot()
	{
		for (uint32_t slot = 0; slot < kPluginComponentSlotCount; ++slot)
		{
			if (m_ComponentSlots[slot])
				continue;
			m_ComponentSlots[slot] = true;
			return slot;
		}
		return kPluginComponentSlotCount;   // 用尽(调用方按可读诊断拒绝注册)
	}

	void PluginManager::ReleaseComponentSlot(uint32_t slot)
	{
		if (slot < kPluginComponentSlotCount)
			m_ComponentSlots[slot] = false;
	}

	// ---- T3b:编辑器扩展(命令 / 面板)---------------------------------------------------

	std::vector<PluginEditorCommand> PluginManager::EditorCommands() const
	{
		std::vector<PluginEditorCommand> commands;
		for (const Record& record : m_Records)
			for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
			{
				PluginEditorCommand command;
				command.PluginId = record.Entry.Manifest.Id;
				command.Id = item.Id;
				command.Label = item.Label;
				command.Tooltip = item.Tooltip;
				command.CommandName = "plugin.command." + command.PluginId + "." + command.Id;
				command.InvokeCount = item.InvokeCount;
				commands.push_back(std::move(command));
			}
		return commands;
	}

	bool PluginManager::FindEditorCommand(const std::string& pluginId, const std::string& id,
		PluginEditorCommand* out) const
	{
		for (const Record& record : m_Records)
		{
			if (record.Entry.Manifest.Id != pluginId)
				continue;
			for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
			{
				if (item.Id != id)
					continue;
				if (out)
				{
					out->PluginId = pluginId;
					out->Id = item.Id;
					out->Label = item.Label;
					out->Tooltip = item.Tooltip;
					out->CommandName = "plugin.command." + pluginId + "." + item.Id;
					out->InvokeCount = item.InvokeCount;
				}
				return true;
			}
		}
		return false;
	}

	bool PluginManager::InvokeEditorCommand(const std::string& command, std::string* error)
	{
		// 两种写法:完整命令名 `plugin.command.<pluginId>.<id>` 或裸 id `<id>`(唯一命中才执行)。
		// 插件 id 与命令 id 都允许含点(反域名习惯)⇒ **不按最后一个点拆解**,
		// 而是按 (pluginId, id) 逐条比对完整名 —— 不会把 `test.editorui.ping` 拆错。
		constexpr const char* kPrefix = "plugin.command.";
		const bool fullName = command.rfind(kPrefix, 0) == 0;
		const std::string id = fullName ? command.substr(std::strlen(kPrefix)) : command;
		if (id.empty())
		{
			if (error) *error = "editor command id is empty";
			return false;
		}

		for (Record& record : m_Records)
		{
			for (RegisteredEditorCommand& item : record.RegisteredEditorCommands)
			{
				const std::string expected = record.Entry.Manifest.Id + "." + item.Id;
				if (item.Id != id && expected != id)
					continue;
				if (!item.Callback)
				{
					if (error) *error = "editor command '" + id + "' has no callback";
					return false;
				}
				// 回调不该抛,但不能让异常把命令面(乃至编辑器)带走 —— 与 LogBridge 同口径。
				try
				{
					item.Callback(item.UserData);
				}
				catch (const std::exception& exception)
				{
					Log(WePluginLogError, record.Entry.Manifest.Id + ": editor command '" + id
						+ "' raised an exception: " + exception.what());
				}
				catch (...)
				{
					Log(WePluginLogError, record.Entry.Manifest.Id + ": editor command '" + id
						+ "' raised an unknown exception");
				}
				++item.InvokeCount;
				Log(WePluginLogInfo, record.Entry.Manifest.Id + ": editor command '" + id
					+ "' invoked (count=" + std::to_string(item.InvokeCount) + ")");
				return true;
			}
		}

		if (fullName)
		{
			size_t matches = 0;
			for (const Record& record : m_Records)
				for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
					if (record.Entry.Manifest.Id + "." + item.Id == id)
						++matches;
			if (matches > 1)
			{
				if (error) *error = "editor command name '" + command + "' is ambiguous";
				return false;
			}
		}
		if (error) *error = "no editor command matches '" + command + "'";
		return false;
	}

	std::vector<PluginEditorPanel> PluginManager::EditorPanels() const
	{
		std::vector<PluginEditorPanel> panels;
		for (const Record& record : m_Records)
			for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
			{
				PluginEditorPanel panel;
				panel.PluginId = record.Entry.Manifest.Id;
				panel.Id = item.Id;
				panel.DisplayId = "plugin.panel." + panel.PluginId + "." + panel.Id;
				panel.Title = item.Title.empty() ? item.Id : item.Title;
				panels.push_back(std::move(panel));
			}
		return panels;
	}

	bool PluginManager::FindEditorPanel(const std::string& panelId, PluginEditorPanel* out) const
	{
		for (const Record& record : m_Records)
			for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
			{
				const std::string displayId = "plugin.panel." + record.Entry.Manifest.Id + "." + item.Id;
				if (displayId != panelId)
					continue;
				if (out)
				{
					out->PluginId = record.Entry.Manifest.Id;
					out->Id = item.Id;
					out->DisplayId = displayId;
					out->Title = item.Title.empty() ? item.Id : item.Title;
				}
				return true;
			}
		return false;
	}

	int PluginManager::RenderEditorPanel(const std::string& panelId) const
	{
		if (!m_EditorHost)
			return -1;
		for (const Record& record : m_Records)
			for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
			{
				const std::string displayId = "plugin.panel." + record.Entry.Manifest.Id + "." + item.Id;
				if (displayId != panelId)
					continue;
				PluginEditorPanel panel;
				panel.PluginId = record.Entry.Manifest.Id;
				panel.Id = item.Id;
				panel.DisplayId = displayId;
				panel.Title = item.Title.empty() ? item.Id : item.Title;
				return m_EditorHost->RenderEditorPanelSurface(panel, item.Draw, item.UserData);
			}
		return -1;
	}

	bool PluginManager::RegisterEditorCommand(Record& record, const WeEditorCommandDesc& desc)
	{
		const std::string& pluginId = record.Entry.Manifest.Id;
		if (desc.StructSize < sizeof(WeEditorCommandDesc) || desc.AbiVersion != WE_PLUGIN_ABI_VERSION)
		{
			Log(WePluginLogWarn, pluginId + ": editor command registration rejected (struct/abi mismatch size="
				+ std::to_string(desc.StructSize) + " abi=" + std::to_string(desc.AbiVersion) + ")");
			return false;
		}
		const std::string id = desc.Id ? desc.Id : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": editor command registration rejected (empty id)");
			return false;
		}
		if (!desc.Callback)
		{
			Log(WePluginLogWarn, pluginId + ": editor command '" + id
				+ "' registration rejected (no callback)");
			return false;
		}
		for (const RegisteredEditorCommand& item : record.RegisteredEditorCommands)
		{
			if (item.Id == id)
			{
				Log(WePluginLogWarn, pluginId + ": editor command '" + id
					+ "' is already registered by this plugin; registration ignored");
				return false;
			}
		}
		if (!m_EditorHost)
		{
			Log(WePluginLogWarn, pluginId + ": editor command '" + id
				+ "' registration rejected (no editor host is wired)");
			return false;
		}
		if (!m_EditorHost->RegisterEditorCommand(pluginId, desc, desc.Callback, desc.UserData))
		{
			Log(WePluginLogWarn, pluginId + ": editor command '" + id
				+ "' registration rejected by the editor");
			return false;
		}

		RegisteredEditorCommand entry;
		entry.Id = id;
		entry.Label = desc.Label ? desc.Label : id;
		entry.Tooltip = desc.Tooltip ? desc.Tooltip : "";
		entry.Callback = desc.Callback;
		entry.UserData = desc.UserData;
		record.RegisteredEditorCommands.push_back(std::move(entry));
		Log(WePluginLogInfo, pluginId + ": registered editor command '" + id + "'");
		return true;
	}

	bool PluginManager::UnregisterEditorCommand(Record& record, const char* rawId)
	{
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string id = rawId ? rawId : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": editor command unregister rejected (empty id)");
			return false;
		}
		const auto tracked = std::find_if(record.RegisteredEditorCommands.begin(),
			record.RegisteredEditorCommands.end(),
			[&id](const RegisteredEditorCommand& item) { return item.Id == id; });
		if (tracked == record.RegisteredEditorCommands.end())
			return true;   // 幂等:本插件没注册过(也可能已被卸载兜底回收)
		if (m_EditorHost)
			m_EditorHost->UnregisterEditorCommand(pluginId, id);
		record.RegisteredEditorCommands.erase(tracked);
		Log(WePluginLogInfo, pluginId + ": unregistered editor command '" + id + "'");
		return true;
	}

	bool PluginManager::RegisterEditorPanel(Record& record, const WeEditorPanelDesc& desc)
	{
		const std::string& pluginId = record.Entry.Manifest.Id;
		if (desc.StructSize < sizeof(WeEditorPanelDesc) || desc.AbiVersion != WE_PLUGIN_ABI_VERSION)
		{
			Log(WePluginLogWarn, pluginId + ": editor panel registration rejected (struct/abi mismatch size="
				+ std::to_string(desc.StructSize) + " abi=" + std::to_string(desc.AbiVersion) + ")");
			return false;
		}
		const std::string id = desc.Id ? desc.Id : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": editor panel registration rejected (empty id)");
			return false;
		}
		if (!desc.Draw)
		{
			Log(WePluginLogWarn, pluginId + ": editor panel '" + id
				+ "' registration rejected (no draw callback)");
			return false;
		}
		for (const RegisteredEditorPanel& item : record.RegisteredEditorPanels)
		{
			if (item.Id == id)
			{
				Log(WePluginLogWarn, pluginId + ": editor panel '" + id
					+ "' is already registered by this plugin; registration ignored");
				return false;
			}
		}
		if (!m_EditorHost)
		{
			Log(WePluginLogWarn, pluginId + ": editor panel '" + id
				+ "' registration rejected (no editor host is wired)");
			return false;
		}
		// 渲染入口的 host 参数 = 本管理器;userData = 该插件记录(卸载前必然注销)。
		if (!m_EditorHost->RegisterEditorPanel(pluginId, desc, &PluginManager::RenderPanelEntry, nullptr))
		{
			Log(WePluginLogWarn, pluginId + ": editor panel '" + id
				+ "' registration rejected by the editor");
			return false;
		}

		RegisteredEditorPanel entry;
		entry.Id = id;
		entry.Title = desc.Title ? desc.Title : id;
		entry.Draw = desc.Draw;
		entry.UserData = desc.UserData;
		record.RegisteredEditorPanels.push_back(std::move(entry));
		Log(WePluginLogInfo, pluginId + ": registered editor panel '" + id + "'");
		return true;
	}

	bool PluginManager::UnregisterEditorPanel(Record& record, const char* rawId)
	{
		const std::string& pluginId = record.Entry.Manifest.Id;
		const std::string id = rawId ? rawId : "";
		if (id.empty())
		{
			Log(WePluginLogWarn, pluginId + ": editor panel unregister rejected (empty id)");
			return false;
		}
		const auto tracked = std::find_if(record.RegisteredEditorPanels.begin(),
			record.RegisteredEditorPanels.end(),
			[&id](const RegisteredEditorPanel& item) { return item.Id == id; });
		if (tracked == record.RegisteredEditorPanels.end())
			return true;   // 幂等
		if (m_EditorHost)
			m_EditorHost->UnregisterEditorPanel(pluginId, id);
		record.RegisteredEditorPanels.erase(tracked);
		Log(WePluginLogInfo, pluginId + ": unregistered editor panel '" + id + "'");
		return true;
	}

	// ---- T4:脚本函数库(账本在 World/Script/PluginScriptLibrary)------------------------

	bool PluginManager::RegisterScriptFunction(Record& record, const WeScriptFunctionDesc& desc)
	{
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

	bool PluginManager::UnregisterScriptFunction(Record& record, const char* name)
	{
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

	void PluginManager::ReclaimEditorExtensions(Record& record)
	{
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

	int PluginManager::RenderPanelEntry(void* host, const PluginEditorPanel& panel,
		WeEditorUiApi* ui, void* uiContext, void* userData)
	{
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
	void PluginManager::WarnContributionDrift(const Record& record)
	{
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

	void PluginManager::ReclaimPluginRegistrations(Record& record,
		World::Schema::SchemaRegistry* schemas)
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

	void PluginManager::Reject(Record& record, std::string reason)
	{
		record.Entry.State = PluginState::Rejected;
		record.Entry.Diagnostic = std::move(reason);
		record.Entry.Order = -1;
		Log(WePluginLogError, "rejected id=" + record.Entry.Manifest.Id + " reason=" + record.Entry.Diagnostic);
	}

	bool PluginManager::Discover(const std::filesystem::path& enginePluginsRoot,
		const std::filesystem::path& projectPluginsRoot,
		const std::vector<std::filesystem::path>& devBinaryRoots)
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

	// ---- HOTR-P3-T8:插件"一键重载"的请求队列 / 定位 / 构建进度 --------------------

	void PluginManager::RequestReloadWithBuild(const std::string& id)
	{
		if (id.empty())
			return;
		// 与 RequestReload 同一个请求队列:登记顺序 + 按 id 去重由它统一保证。
		RequestReload(id);
		if (std::find(m_ReloadBuildRequests.begin(), m_ReloadBuildRequests.end(), id)
			== m_ReloadBuildRequests.end())
			m_ReloadBuildRequests.push_back(id);
	}

	std::vector<PluginManager::PluginReloadRequest> PluginManager::ConsumeReloadRequestsDetailed()
	{
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

	std::filesystem::path PluginManager::PluginSourceDir(const std::string& id) const
	{
		const Record* record = FindRecord(id);
		return record ? record->Entry.Manifest.Root : std::filesystem::path();
	}

	std::string PluginManager::PluginBuildTarget(const std::string& id) const
	{
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

	void PluginManager::BeginPluginBuild(const std::string& id)
	{
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

	void PluginManager::CompletePluginBuild(const std::string& id, int exitCode,
		const std::string& output)
	{
		PluginBuildProgress& progress = m_PluginBuilds[id];
		progress.Running = false;
		progress.ExitCode = exitCode;
		progress.Output = output;
	}

	PluginManager::PluginBuildProgress PluginManager::PluginBuildState(const std::string& id) const
	{
		const auto found = m_PluginBuilds.find(id);
		return found == m_PluginBuilds.end() ? PluginBuildProgress() : found->second;
	}

	bool PluginManager::PluginBuildRunning() const
	{
		for (const auto& entry : m_PluginBuilds)
			if (entry.second.Running)
				return true;
		return false;
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

	// PLUG-T5:发行形态加载(见 PluginManager.h 的契约说明)。
	PluginManager::Status PluginManager::LoadPackaged(const std::filesystem::path& libraryDir,
		const std::vector<std::string>& orderedIds, WorldContext& context, std::string* error)
	{
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
