#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	// P2 W5-L1:资产外部改动的内容指纹与轮询监听(材质 .wmat / 贴图 / 场景 .wd 通用)。
	//
	// 与 Script/HotReload 的口径一致:**内容哈希优先**(同内容重写只动 mtime 不算变化);
	// 内容读不到时退化为 "size|mtime" 兜底(Exists=true, FromContent=false)。
	// 读取走 VFS 优先、磁盘回退(内容根 = WLD_ASSETPATH,与材质路径书写约定一致)。
	// M3:`.wmat` 是材质实例,指纹 = 本文件内容 ⊕ **解析后父级链**的指纹
	// (父级改 → 子材质指纹变化 → 缓存/热重载一起失效;父级链断裂同样改变指纹)。
	struct WLD_API AssetFingerprint
	{
		uint64_t Value = 0;
		bool Exists = false;
		bool FromContent = false;   // true = 内容哈希;false = size|mtime 兜底
	};

	// VFS 优先、磁盘回退的字节读取(逻辑路径相对内容根;供 .wmat/.png/.wd 通用)。
	WLD_API bool ReadAssetBytes(const std::string& logicalPath, std::vector<uint8_t>& out,
		std::string* error = nullptr);
	WLD_API AssetFingerprint FingerprintAsset(const std::string& logicalPath,
		std::string* error = nullptr);

	// 轮询监听,与 ScriptFileWatch 同语义:
	//   - Watch() 建立基线(首次登记不产生变化);重复登记 = 用当前内容重置基线;
	//   - Poll(dt) 累计"连续稳定 debounce 秒"的变化(期间内容再变则重启计时);
	//   - 同内容重写(只动 mtime)不报告;内容改回已确认值 → 撤销未决变化;
	//   - 报告后该内容即成为新基线(宿主无需再 Watch);
	//   - 返回路径按字典序升序。
	class WLD_API AssetFileWatch
	{
	public:
		// 150ms:与脚本热重载同一节拍(材质/场景用;贴图用 500ms 独立实例)。
		static constexpr double kDefaultDebounceSeconds = 0.15;

		AssetFileWatch() = default;
		explicit AssetFileWatch(double debounceSeconds);

		void Watch(const std::string& logicalPath);
		void Unwatch(const std::string& logicalPath);
		void Clear();

		std::size_t Size() const { return m_Entries.size(); }
		bool IsWatched(const std::string& logicalPath) const;
		double DebounceSeconds() const { return m_DebounceSeconds; }
		// 当前监听集合(升序):宿主用它做"集合同步"(新路径 Watch、消失路径 Unwatch)。
		std::vector<std::string> WatchedPaths() const;

		std::vector<std::string> Poll(double deltaSeconds);

	private:
		struct Entry
		{
			AssetFingerprint Stable;    // 已确认(不报告)的指纹
			AssetFingerprint Pending;   // 未决内容
			double Elapsed = 0.0;       // Pending 已稳定持续的秒数
			bool HasPending = false;
		};

		std::unordered_map<std::string, Entry> m_Entries;
		double m_DebounceSeconds = kDefaultDebounceSeconds;
	};

	struct WLD_API AssetReloadFailure
	{
		std::string Path;
		std::string Error;
	};

	// 一次 PollAssetChanges 的合并报告(路径升序;无变化时全空)。
	struct WLD_API AssetHotReloadReport
	{
		std::vector<std::string> ReloadedMaterials;       // 磁盘变了且 buffer 干净 → 已原地重载
		std::vector<std::string> SkippedDirtyMaterials;   // 磁盘变了但有未保存修改 → 只报告
		std::vector<AssetReloadFailure> FailedMaterials;  // 读取/解析失败:保留旧内存态
		std::vector<std::string> InvalidatedTextures;     // 内容变化的贴图(GPU 缓存已失效)
		// M4-S3:内容变化的 `.hlsl` 表面函数资产。引用它们的材质会被刷新参数表 +
		// Revision 自增(渲染侧重建参数 UBO/表面描述符集);**重新编译与
		// MaterialSurfaceRuntime::Install 由编辑器侧执行**(内核不在渲染线程里跑编译器)。
		std::vector<std::string> ChangedShaders;

		bool Any() const
		{
			return !ReloadedMaterials.empty() || !SkippedDirtyMaterials.empty()
				|| !FailedMaterials.empty() || !InvalidatedTextures.empty()
				|| !ChangedShaders.empty();
		}
	};
}
