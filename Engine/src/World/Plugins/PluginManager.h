#pragma once

#include "World/Core/Asset/AssetImporter.h"
#include "World/Plugins/PluginManifest.h"
#include "World/Plugins/PluginHostServices.h"
#include "World/Plugins/PluginComponentStorage.h"
#include "World/Schema/Schema.h"
#include "World/Utils/DynamicLibrary.h"

#include <array>
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
			// T2c:Unload 时它的 blob 组件在活场景里还有实例 —— 干净拒绝(先移除组件 /
			// 销毁场景),避免把场景数据变成"没有 schema 的孤儿"。
			HasLiveInstances,
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

		// ---- PLUG-T5:发行形态加载(bin/plugins/*.dll 平铺目录)--------------------------
		//
		// cook 的发行布局把闭包内插件的 DLL 拷到 `<publish>/bin/plugins/<name>.dll`,并把这些
		// 插件的 **id 按依赖拓扑序**写进发行清单的 `plugins.shipped`。Runtime 按这份清单加载:
		//   * 每个 DLL 先 `LoadLibrary` + `WePluginQuery` 读出 id,再走与发现式加载**同一套**
		//     契约校验(ABI / StructSize / id 一致 / Register / 回滚)——不复制第二套加载器;
		//   * 清单里列了、目录里没有 ⇒ 记 ERROR 并计入失败(**不静默**,不阻断其余插件);
		//   * 目录里多余的 DLL(清单没列)⇒ 记 WARN 后忽略(**不静默**);
		//   * 依赖顺序取清单顺序(cook 已写成拓扑序);单个失败不影响其余。
		// 返回 Ok = 清单里的插件全部加载成功;Rejected = 有缺失/失败(error 给首条原因)。
		// 与 Discover 一样:已有 Loaded 条目时拒绝(先 UnloadAll)。
		Status LoadPackaged(const std::filesystem::path& libraryDir,
			const std::vector<std::string>& orderedIds, WorldContext& context,
			std::string* error = nullptr);

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

		// ---- T3b:编辑器扩展面(命令 / 面板)--------------------------------------------
		//
		// 账本语义与 T2 注册面一致:插件通过 WeHostApi::RegisterEditorCommand /
		// RegisterEditorPanel 注册,卸载 / Register false / Register 抛异常 / 管理器析构
		// 四条路径都会兜底回收(未自己注销则记 WARN),不留指向已释放 DLL 的回调。
		// 真正的"进编辑器命令面 / 面板注册表"由宿主实现 PluginEditorHost 完成
		// (Editor 的 PluginEditorHost);未接线时注册被干净拒绝(false + 警告)。
		//
		// 生命周期契约:宿主对象**必须比本管理器活得久**(或宿主析构前先
		// SetEditorHost(nullptr) 断开)—— 本管理器只保存非拥有指针,析构时的兜底回收
		// 会回拨宿主注销命令/面板。EditorLayer 的收尾顺序满足这条:先
		// UnloadAll(登记随之清空)+ SetEditorHost(nullptr),再释放管理器。
		void SetEditorHost(PluginEditorHost* host) { m_EditorHost = host; }
		PluginEditorHost* EditorHost() const { return m_EditorHost; }

		// 已注册命令快照(确定性顺序 = 插件发现顺序 + 注册顺序)。
		std::vector<PluginEditorCommand> EditorCommands() const;
		// 单条查询(未命中 = 返回 false,不改 *out)。
		bool FindEditorCommand(const std::string& pluginId, const std::string& id,
			PluginEditorCommand* out) const;
		// 触发一条命令(宿主命令面入口;`plugin.command.<pluginId>.<id>` 也接受)。
		// 成功 = 插件回调被调用(计数 +1,异常被截获并记 ERROR 后仍返回 true —— 回调跑了
		// 就是跑了,异常属于插件契约违约,不让它把命令面带走)。
		bool InvokeEditorCommand(const std::string& command, std::string* error = nullptr);
		// 已注册面板快照(确定性顺序)与查找。
		std::vector<PluginEditorPanel> EditorPanels() const;
		bool FindEditorPanel(const std::string& panelId, PluginEditorPanel* out) const;
		// 渲染一个插件面板(宿主面板层调用;返回 0 = 正常,非 0 = 面板缺失 / 未接线 / 插件违约)。
		int RenderEditorPanel(const std::string& panelId) const;

		// ---- T4:脚本函数库(插件经 WeHostApi::RegisterScriptFunction 注册的全局函数)----
		// 账本 + 运行时绑定 + 存根渲染的唯一事实源在 World/Script/PluginScriptLibrary.h;
		// 这里只暴露"本管理器已加载插件"的可观测快照(确定性顺序 = 条目顺序 + 注册顺序)。
		struct PluginScriptFunction
		{
			std::string PluginId;
			std::string Name;       // 完整点分名("hello.ping")
			std::string Namespace;  // 全局表名
			std::string Member;     // 表里的函数名
			std::string Signature;  // 注册时的存根签名原文(可空)
			std::string Doc;
		};
		std::vector<PluginScriptFunction> ScriptFunctions() const;
		// 单条查询(未命中 = 返回 false,不改 *out)。
		bool FindScriptFunction(const std::string& pluginId, const std::string& name,
			PluginScriptFunction* out) const;

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
			// ---- T2c:存储桥(仅声明了 Size > 0 的组件非空)----
			// unique_ptr:Schema.Storage 必须指向**地址稳定**的绑定(条目会被搬动,
			// 注册表里的 TypeSchema 拷贝也持有同一指针),所以绑定放在堆上单独持有。
			std::unique_ptr<World::Schema::StorageBinding> Storage;
			uint32_t ComponentSlot = 0;            // 在册槽位(id 段的低 8 位)
			uint32_t DeclaredSize = 0;             // 插件声明的结构总大小(诊断用)
		};
		// T3b 注册账本的一条:插件注册的编辑器命令 / 面板(回调与 userData 取自插件描述)。
		struct RegisteredEditorCommand
		{
			std::string Id;
			std::string Label;
			std::string Tooltip;
			WeEditorCommandCallback Callback = nullptr;
			void* UserData = nullptr;
			uint64_t InvokeCount = 0;
		};
		struct RegisteredEditorPanel
		{
			std::string Id;
			std::string Title;
			WeEditorPanelDrawFn Draw = nullptr;
			void* UserData = nullptr;
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
			// T3b:本插件注册的编辑器命令 / 面板(顺序 = 注册顺序)。
			std::vector<RegisteredEditorCommand> RegisteredEditorCommands;
			std::vector<RegisteredEditorPanel> RegisteredEditorPanels;
			// T4:本插件注册的脚本函数(完整名;顺序 = 注册顺序)。
			std::vector<std::string> RegisteredScriptFunctions;
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
		// T3b:编辑器命令 / 面板的注册 / 注销(WeHostApi 尾部字段的宿主实现;
		// 真正的编辑器接线转发给 PluginEditorHost)。
		bool RegisterEditorCommand(Record& record, const WeEditorCommandDesc& desc);
		bool UnregisterEditorCommand(Record& record, const char* id);
		bool RegisterEditorPanel(Record& record, const WeEditorPanelDesc& desc);
		bool UnregisterEditorPanel(Record& record, const char* id);
		// T4:脚本函数的注册 / 注销(WeHostApi 尾部字段的宿主实现;账本在 PluginScriptLibrary)。
		bool RegisterScriptFunction(Record& record, const WeScriptFunctionDesc& desc);
		bool UnregisterScriptFunction(Record& record, const char* name);
		// 卸载/失败回滚的兜底:插件没自己注销的资产类型/导入器/组件类型在这里移除并记警告
		// (不留悬空回调;组件类型整模块注销 + 释放字段访问器槽位)。
		// schemas = 组件的 schema 注册表(可空:空则只清账本/槽位并记 ERROR)。
		void ReclaimPluginRegistrations(Record& record,
			World::Schema::SchemaRegistry* schemas = nullptr);
		// PLUG-T5:声明(`plugin.we.yaml` 的 contributes)与运行时实际注册项的比对 ——
		// 只对**声明了 contributes** 的插件生效;不一致逐条记 WARN(打包索引会漏报,必须可观测)。
		void WarnContributionDrift(const Record& record);
		// 组件类型整模块注销的兜底(卸载/回滚/管理器析构共用)。
		void ReclaimComponentTypes(Record& record, World::Schema::SchemaRegistry* schemas);
		// T2c:插件组件在册槽位(存储 id 段的低位;空槽 = 可分配)。
		// 槽位只决定 id 的低位,档位位由声明大小决定 —— 复用槽位换档位 = 换 id。
		uint32_t AllocateComponentSlot();
		void ReleaseComponentSlot(uint32_t slot);
		// T3b:命令 / 面板的兜底回收(卸载 / Register false / 抛异常 / 析构四条路径共用)。
		void ReclaimEditorExtensions(Record& record);
		// WeHostApi 函数指针桥:userData → HostApiBox → 转发(越界/空参一律干净失败)。
		static bool BridgeRegisterAssetType(void* userData, const WeAssetTypeDesc* desc);
		static bool BridgeUnregisterAssetType(void* userData, const char* id);
		static bool BridgeRegisterAssetImporter(void* userData, const WeAssetImporterDesc* desc);
		static bool BridgeUnregisterAssetImporter(void* userData, const char* id);
		static void* BridgeLookupExport(void* userData, const char* pluginId, const char* name,
			uint32_t minVersion);
		static bool BridgeRegisterComponent(void* userData, const WeComponentDesc* desc);
		static bool BridgeUnregisterComponent(void* userData, const char* id);
		static bool BridgeRegisterEditorCommand(void* userData, const WeEditorCommandDesc* desc);
		static bool BridgeUnregisterEditorCommand(void* userData, const char* id);
		static bool BridgeRegisterEditorPanel(void* userData, const WeEditorPanelDesc* desc);
		static bool BridgeUnregisterEditorPanel(void* userData, const char* id);
		static bool BridgeRegisterScriptFunction(void* userData, const WeScriptFunctionDesc* desc);
		static bool BridgeUnregisterScriptFunction(void* userData, const char* name);
		// T3b:交给宿主的渲染入口(宿主只存函数指针;userData = 插件记录,卸载前必然注销)。
		static int RenderPanelEntry(void* host, const PluginEditorPanel& panel,
			WeEditorUiApi* ui, void* uiContext, void* userData);
		static void LogBridge(void* userData, int level, const char* message);
		static void Log(int level, const std::string& text);

		std::vector<Record> m_Records;
		std::vector<size_t> m_LoadSequence;  // 发现期算出的拓扑序(LoadAll 用)
		std::vector<std::filesystem::path> m_DevBinaryRoots;   // 开发构建产物根(Discover 传入)
		// T2c:插件组件存储的在册槽位(true = 已占用)。容量 = kPluginComponentSlotCount。
		std::array<bool, kPluginComponentSlotCount> m_ComponentSlots {};
		WeHostApi m_HostApi;
		int m_NextOrder = 0;
		// Register 调用期间"当前有效的宿主表"(插件在 Register 里调宿主注册面时用它解析归属)。
		HostApiBox* m_ActiveBox = nullptr;
		// T3b:编辑器接线(非拥有;Editor 在插件加载前 SetEditorHost,nullptr = 未接线)。
		PluginEditorHost* m_EditorHost = nullptr;
	};
}
