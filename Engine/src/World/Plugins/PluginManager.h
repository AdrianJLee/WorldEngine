#pragma once

#include "World/Plugins/PluginManifest.h"
#include "World/Utils/DynamicLibrary.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	class WorldContext;
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
	// T1 边界:不做能力注册面(资产类型/组件/命令/渲染钩子 = T2/T3)、不做打包(T5)、
	// 不做热重载(T6)。
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
		bool Discover(const std::filesystem::path& enginePluginsRoot,
			const std::filesystem::path& projectPluginsRoot);

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

	private:
		// 每次成功加载的宿主侧状态:宿主表 + 日志前缀用的插件 id(插件只原样回传 UserData)。
		struct HostApiBox
		{
			std::string PluginId;
			WeHostApi Api;
		};
		struct Record
		{
			PluginEntry Entry;
			std::unique_ptr<World::DynamicLibrary> Library;
			const WePlugin* Plugin = nullptr;
			std::unique_ptr<HostApiBox> Host;
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
		static void LogBridge(void* userData, int level, const char* message);
		static void Log(int level, const std::string& text);

		std::vector<Record> m_Records;
		std::vector<size_t> m_LoadSequence;  // 发现期算出的拓扑序(LoadAll 用)
		WeHostApi m_HostApi;
		int m_NextOrder = 0;
	};
}
