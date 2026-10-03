#include "wldpch.h"
#include "World/Plugins/PluginManager.h"
#include "World/Asset/AssetTypeRegistry.h"
#include "World/Core/WorldContext.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Scene/Scene.h"
#include "World/Script/Runtime/PluginScriptLibrary.h"
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
namespace PluginManagerDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace PluginManagerDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace PluginManagerDetail
	{
std::string Hex32(uint32_t value);

std::string JoinIds(const std::set<std::string>& ids);

std::string ResolveComponentDisplayName(const std::string& pluginId, const std::string& typeId, const std::string& declared);


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
extern ComponentFieldSlot g_ComponentFieldSlots[kMaxComponentFieldSlots];

uint32_t AllocateComponentFieldSlot(uint32_t kind, uint32_t offset);

void ReleaseComponentFieldSlot(uint32_t slot);

uint32_t ComponentFieldKindSize(uint32_t kind);


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
bool ReadComponentFieldValue(uint32_t kind, const uint8_t* address, World::Schema::Value* out);

bool WriteComponentFieldValue(uint32_t kind, uint8_t* address, const World::Schema::Value& value);


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
World::Schema::ModuleId PluginSchemaModule(const std::string& pluginId);


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
}
