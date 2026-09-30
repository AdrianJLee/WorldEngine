#pragma once

#include "World/Core/Asset/AssetImporter.h"
#include "World/Plugins/PluginManifest.h"
#include "World/Schema/Schema.h"
#include "World/Utils/DynamicLibrary.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	class WorldContext;

	namespace Schema
	{
		class SchemaRegistry;
	}
}

namespace World::Plugins
{
	// 一个插件在管理器里的可观测条目(插件管理器面板 / 诊断 / 单测的数据源)。
	struct PluginEntry
	{
		PluginManifest Manifest;
		PluginState State = PluginState::Discovered;
		std::string Diagnostic;  // State == Rejected 时的可读原因
		int Order = -1;          // State == Loaded 时的加载顺序(0 起,拓扑序)
		uint32_t PluginAbi = 0;  // 插件自报的 ABI(诊断用;契约校验前失败时为 0)
		uint32_t PluginStructSize = 0;  // 插件自报的 StructSize(诊断/前向兼容判据用)
		// 插件 WePlugin::Exports 的 Name 列表(加载成功后填充;面板/`plugin.info` 直接展示)。
		std::vector<std::string> ExportNames;
	};

	// 插件加载器(对外主入口):发现 → 校验干净拒绝 → 依赖拓扑 → 加载 → 卸载。
	//
	// 生命周期契约(与 World/Plugins/WePluginApi.h 同源):
	//   * Register 返回 false = 插件声明"本次加载失败且自己已清干净" —— 宿主**不**调用
	//     Unregister(避免二次释放),直接释放 DLL;
	//   * Register 抛异常 = 契约违约且可能留了半注册状态 —— 宿主 best-effort 调用
	//     Unregister 回滚,再释放 DLL;
	//   * 成功加载过的插件在卸载时 Unregister **恰好调用一次**;
	//   * 单个插件失败不影响其余插件(逐条目诊断在 Entries()/Find(),不做整批回滚)。
	//
	// T2 边界:注册面只到"资产类型 + 导入器 + C++ 导出查询";组件/系统/命令/面板/渲染钩子
	// 与打包(T5)、热重载(T6)不在本类内。
	class PluginManager
	{
	public:
		enum class Status : uint8_t
		{
			Ok = 0,
			NotFound,             // 没有该 id 的插件
			LoadFailed,           // DLL 打不开 / 产物缺失
			Rejected,             // 清单/契约校验失败,或 Register 失败
			AlreadyLoaded,        // Load 时该插件已加载
			DependencyNotLoaded,  // Load 该插件时它的依赖还没加载
			HasLoadedDependents,  // Unload 时还有已加载插件依赖它
			NotLoaded,            // Unload 未加载的插件
		};
		static const char* StatusName(Status status);

		PluginManager();
		~PluginManager();
		PluginManager(const PluginManager&) = delete;
		PluginManager& operator=(const PluginManager&) = delete;

		// 发现两个根下的 `<root>/<name>/plugin.we.yaml`(位置即 scope:engineRoot → Engine,
		// projectRoot → Project)。根不存在 = 0 个插件(不是错误)。
		// 清单/位置/重复 id/缺依赖/依赖环在发现期逐条给诊断(条目 State=Rejected);
		// 有插件仍处于 Loaded 时拒绝重新发现(先 UnloadAll),返回 false 且保持现状。
		//
		// devBinaryRoots(2026-09-30):**开发构建**的插件产物根(如
		// `<repo>/build/x64-Debug/bin/Debug/plugins/Debug`)。插件包自带 `<Root>/bin/<name>.dll`
		// 不存在时按顺序在这些根里找 `<root>/<name>.dll` 并记为 LibraryPath ——
		// 引擎插件由引擎构建产出,源码树里不写产物(发布布局仍以自带 bin/ 为准)。
		bool Discover(const std::filesystem::path& enginePluginsRoot,
			const std::filesystem::path& projectPluginsRoot,
			const std::vector<std::filesystem::path>& devBinaryRoots = {});

		// 按依赖拓扑加载全部未加载插件;单条失败继续加载其余插件。
		// 返回 Ok = 全部成功(含 0 个);Rejected = 有条目被拒绝(逐条诊断见 Find()/Entries())。
		Status LoadAll(WorldContext& context, std::string* error = nullptr);
		// 加载单个插件;依赖必须已加载(DependencyNotLoaded)。
		Status Load(const std::string& id, WorldContext& context, std::string* error = nullptr);
		// 卸载单个插件:Unregister 恰好一次 + 释放 DLL;有已加载依赖者 = HasLoadedDependents。
		Status Unload(const std::string& id, WorldContext& context, std::string* error = nullptr);
		// 按加载顺序**逆序**卸载全部(与 ModuleManager::UnloadAll 同口径:依赖者先走)。
		void UnloadAll(WorldContext& context);

		// 已发现的插件总数(含被拒绝的)/ 已加载数。
		size_t Count() const { return m_Records.size(); }
		size_t LoadedCount() const;
		// 条目快照(面板/测试遍历用;运行句柄不暴露)。
		std::vector<PluginEntry> Entries() const;
		const PluginEntry* Find(const std::string& id) const;
		// 已加载插件的 id 列表,顺序即加载顺序(= 依赖拓扑序)。
		std::vector<std::string> LoadOrder() const;
		// 传给插件 Register 的宿主能力表(T1 = 最小集合;字段尾部追加见 WePluginApi.h)。
		const WeHostApi& HostApi() const { return m_HostApi; }

		// ---- T2:能力注册面(WeHostApi 尾部字段的宿主实现;宿主代码也可直接调用)----

		// 按 id + 名字 + 最低版本(含)查已加载插件的 C++ 导出表(WePlugin::Exports)。
		// 未命中 = nullptr(查询失败是正常分支,不记日志)。
		void* LookupExport(const std::string& pluginId, const std::string& name,
			uint32_t minVersion) const;

		// 已加载插件贡献的导入器(确定性顺序 = 条目顺序)。
		// 现状:导入面没有单例注册表 —— `BuiltinImporters.h` 的 DefaultImporters() 返回一份
		// 内置清单,由宿主(EditorCooker/测试)交给 CookPipeline。因此插件导入器由宿主把
		// **这一份**追加进同一清单(兜底 PassThrough 之前);这里不复制第二套内置注册表。
		// 生命周期:返回的适配器回调指向插件 DLL —— 调用方不得跨 Unload/UnloadAll 持有或使用。
		std::vector<std::shared_ptr<World::Asset::IAssetImporter>> PluginImporters() const;

	private:
		// 每次成功加载的宿主侧状态:宿主表 + 日志前缀用的插件 id(插件只原样回传 UserData)。
		struct HostApiBox
		{
			std::string PluginId;
			PluginManager* Manager = nullptr;
			WeHostApi Api;
			// T2b:本次加载的 WorldContext 里那张 schema 注册表(Register/Unregister 期间刷新)。
			// 组件 schema 必须注册到宿主唯一的注册表实例上,而 WeHostApi 回调只拿得到本盒子,
			// 所以在这里记住归属(生命周期 = WorldContext 的 Schemas() 成员,见 WorldContext.h)。
			World::Schema::SchemaRegistry* Schemas = nullptr;
		};
		// T2 注册账本的一条:插件注册的导入器(id + 适配对象)。
		struct RegisteredImporter
		{
			std::string Id;
			std::shared_ptr<World::Asset::IAssetImporter> Importer;
		};
		// T2b 注册账本的一条:插件注册的组件类型(schema 拷贝 + 它占用的字段访问器槽位)。
		// Schema 拷贝用于"注销单个类型后把其余类型按原样重新注册回同一模块"。
		struct RegisteredComponent
		{
			std::string Id;                        // 类型全名(= 注册键 = registry 的 Id.Name)
			World::Schema::TypeSchema Schema;      // 注册时提交的 schema(含字段访问器)
			std::vector<uint32_t> Slots;           // 该类型字段占用的访问器槽位(注销时释放)
		};
		struct Record
		{
			PluginEntry Entry;
			std::unique_ptr<World::DynamicLibrary> Library;
			const WePlugin* Plugin = nullptr;
			std::unique_ptr<HostApiBox> Host;
			// T2:本插件经 WeHostApi 注册过、尚未注销的项(卸载兜底回收 + 诊断用)。
			std::vector<std::string> RegisteredAssetTypes;
			std::vector<RegisteredImporter> RegisteredImporters;
			// T2b:本插件注册的组件类型(顺序 = 注册顺序)。
			std::vector<RegisteredComponent> RegisteredComponents;
		};

		Record* FindRecord(const std::string& id);
		const Record* FindRecord(const std::string& id) const;
		void ScanRoot(const std::filesystem::path& root, PluginScope scope);
		void ValidateDependencies();
		// 在拓扑排不出的剩余节点里找一条环,返回 "a -> b -> a" 形式的可读链路。
		static std::string CycleChain(const std::vector<Record>& records, const std::vector<size_t>& nodes);
		Status LoadRecord(size_t index, WorldContext& context, std::string* error);
		Status UnloadRecord(Record& record, WorldContext& context, std::string* error, bool enforceDependents);
		void Reject(Record& record, std::string reason);
		// 解析插件调用宿主表时回传的 userData:只接受本管理器在 Register 期间交给插件的表,
		// 或该条目自己的表(Unregister 期间);其它一律视为无效句柄。
		Record* ResolveHostRecord(HostApiBox& box);
		bool RegisterAssetType(Record& record, const WeAssetTypeDesc& desc);
		bool UnregisterAssetType(Record& record, const char* id);
		bool RegisterAssetImporter(Record& record, const WeAssetImporterDesc& desc);
		bool UnregisterAssetImporter(Record& record, const char* id);
		// T2b:组件 schema 注册/注销(WeHostApi 尾部字段的宿主实现)。registry = 本次调用
		// 归属的注册表(由桥从 HostApiBox 取;Register 期间 record.Host 还没建立)。
		bool RegisterComponent(Record& record, World::Schema::SchemaRegistry* registry,
			const WeComponentDesc& desc);
		bool UnregisterComponent(Record& record, World::Schema::SchemaRegistry* registry,
			const char* id);
		// 卸载/失败回滚的兜底:插件没自己注销的资产类型/导入器/组件类型在这里移除并记警告
		// (不留悬空回调;组件类型整模块注销 + 释放字段访问器槽位)。
		// schemas = 组件的 schema 注册表(可空:空则只清账本/槽位并记 ERROR)。
		void ReclaimPluginRegistrations(Record& record,
			World::Schema::SchemaRegistry* schemas = nullptr);
		// 组件类型整模块注销的兜底(卸载/回滚/管理器析构共用)。
		void ReclaimComponentTypes(Record& record, World::Schema::SchemaRegistry* schemas);
		// WeHostApi 函数指针桥:userData → HostApiBox → 转发(越界/空参一律干净失败)。
		static bool BridgeRegisterAssetType(void* userData, const WeAssetTypeDesc* desc);
		static bool BridgeUnregisterAssetType(void* userData, const char* id);
		static bool BridgeRegisterAssetImporter(void* userData, const WeAssetImporterDesc* desc);
		static bool BridgeUnregisterAssetImporter(void* userData, const char* id);
		static void* BridgeLookupExport(void* userData, const char* pluginId, const char* name,
			uint32_t minVersion);
		static bool BridgeRegisterComponent(void* userData, const WeComponentDesc* desc);
		static bool BridgeUnregisterComponent(void* userData, const char* id);
		static void LogBridge(void* userData, int level, const char* message);
		static void Log(int level, const std::string& text);

		std::vector<Record> m_Records;
		std::vector<size_t> m_LoadSequence;  // 发现期算出的拓扑序(LoadAll 用)
		std::vector<std::filesystem::path> m_DevBinaryRoots;   // 开发构建产物根(Discover 传入)
		WeHostApi m_HostApi;
		int m_NextOrder = 0;
		// Register 调用期间"当前有效的宿主表"(插件在 Register 里调宿主注册面时用它解析归属)。
		HostApiBox* m_ActiveBox = nullptr;
	};
}
