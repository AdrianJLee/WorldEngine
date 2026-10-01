#pragma once

#include "World/Core/Asset/ModelImportSettings.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace World
{
	namespace Editor
	{
		// HOTR-P3-T9:内容根下 glTF/GLB **导入源**(`.gltf/.glb`)被外部改动时,
		// 为"已经导入过"的 `.wmodel` 自动重导入 —— 与模型预览面板的 Reimport 同一条
		// 底层路径(`GltfImporter::ImportAsBytes` 内核 + 该 `.wmodel` 的导入设置),
		// 面板没打开时也生效。生效矩阵与开关:`docs/dev/hot-reload.md`。
		//
		// 动机:`.wmodel` 只在"用户点 Apply/Reimport"或 cook 时重建;源(Blender 等外部工具
		// 导出的 `.gltf/.glb`)改了之后,运行中的编辑器仍读旧资产,只能手动 Reimport。
		//
		// 口径(与 `TextureImportWatch` 同一套线程模型,不新造):
		//   * **2s 重扫**内容根下 `**/*.wmodel`(防御上限 kMaxWatchedModels);每个资产的
		//     源路径/导入设置从资产自身读(`WModelIO::ReadMeta` / `ModelImportSettings::
		//     ResolveForImport`,与面板 `ResolveSource` 同一来源:meta.SourcePath 优先,
		//     旧产物按"同目录同名 .gltf/.glb"兜底);**源文件不存在 = 不监听**;
		//   * **内容哈希优先**:同内容重写(只动 mtime)不算变化;stat 先行,stat 没变不读盘;
		//   * **稳定窗口**:变化内容连续稳定 kStableSeconds 秒后才入队(期间再变则重启计时);
		//   * **工作线程导入**:`ImportAsBytes`(与 cook/面板同一条内核,CPU-only)按该 `.wmodel`
		//     的设置与目录产出全部字节;产物先在**同目录临时文件**里写完;
		//   * **主线程提交**:校验目标 `.wmodel` 路径与入队时基线没被别人改写 → 逐个
		//     `MoveFileEx(REPLACE_EXISTING)` 原子替换(模型最后落盘,永不半覆盖);随后
		//     `Mesh::ClearWModelCache()` + `AnimationSystem::ClearCache()` + 日志;
		//   * **冲突门(只跳过、不覆盖)**:对应 `.wmodel` 不存在(从未导入)的源根本不在
		//     监听集合里;对应面板正打开该 `.wmodel` 时跳过本次提交(面板脏状态在面板私有
		//     状态里拿不到 ⇒ 按"面板打开即跳过"),跳过不丢:登记为未决,面板关闭后重试。
		//
		// 开关:`WLD_MODEL_HOTRELOAD=0` 单独关闭(默认开);`WLD_ASSET_HOTRELOAD=0` 一并关
		// (调用点在 EditorLayer::PollAssetHotReload 的同一道闸门之内)。
		class ModelImportWatch
		{
		public:
			ModelImportWatch() = default;
			~ModelImportWatch();

			ModelImportWatch(const ModelImportWatch&) = delete;
			ModelImportWatch& operator=(const ModelImportWatch&) = delete;

			// 冲突门探针(主线程,帧边界调用):返回 true = 该 `.wmodel` 对应面板正打开,
			// 本次提交跳过。空 = 不做面板门(仅用于测试/无壳形态)。
			using SkipProbe = std::function<bool(const std::string& modelLogicalPath)>;
			void SetSkipProbe(SkipProbe probe) { m_SkipProbe = std::move(probe); }

			// 主线程,帧边界(资产热重载的同一道开关内):重扫 + 指纹轮询 + 稳定变化入队。
			void Poll(double deltaSeconds);
			// 主线程,帧边界:取回工作线程备好的字节 → 原子替换 + 清缓存 + 日志。
			void Pump();
			// 停止并 join 工作线程(幂等;OnDetach 与析构都会调)。
			void Shutdown();

			// 重扫内容根(发现新增 / 消失的 `.wmodel`)的周期(秒)。
			static constexpr double kRescanSeconds = 2.0;
			// 变化内容必须稳定满这个秒数才入队(与 AssetFileWatch / TextureImportWatch 同一口径)。
			static constexpr double kStableSeconds = 2.0;
			// 被跳过(面板打开)的未决重导入的重试节流(秒)。
			static constexpr double kDeferredRetrySeconds = 0.5;
			// 防御上限:一次重扫最多登记多少个 `.wmodel`(异常目录不拖爆每帧开销)。
			static constexpr std::size_t kMaxWatchedModels = 4096;

			// 诊断(主线程读;日志/报告用)。
			std::size_t WatchedModelCount() const { return m_Models.size(); }
			std::size_t WatchedSourceCount() const { return m_Sources.size(); }
			std::size_t DeferredCount() const { return m_Deferred.size(); }

		private:
			// 一个被监听源文件的指纹状态机(stat 先行 + 内容哈希 + 稳定窗口)。
			struct Fingerprint
			{
				std::uintmax_t Size = 0;
				std::filesystem::file_time_type Mtime {};
				bool StampValid = false;      // false = 还没建立基线(下一次轮询建立)
				bool HashKnown = false;       // 基线/已确认的内容哈希是否有效
				std::string Hash;             // 已确认(不报告)的内容哈希
				std::string PendingHash;      // 未决内容哈希
				std::uintmax_t PendingSize = 0;
				std::filesystem::file_time_type PendingMtime {};
				bool Pending = false;
				double PendingSeconds = 0.0;
			};

			// 一个"已导入过"的资产:`.wmodel`(目标)+ 它的导入源。
			struct WatchModel
			{
				std::string AssetLogical;     // 如 "models/probe.wmodel"(相对内容根)
				std::string SourceLogical;    // 如 "models/probe.gltf"(空 = 无源,不监听)
				std::filesystem::path SourceAbsolute;
			};

			// `.wmodel` 的 stat + 解析结果缓存:重扫时 stat 没变就不再读 meta(资产可能很大,
			// ReadMeta 会把整份文件读进内存)。
			struct ModelStamp
			{
				std::uintmax_t Size = 0;
				std::filesystem::file_time_type Mtime {};
				bool Valid = false;
				std::string SourceLogical;    // 空 = 这个资产没有可监听的源
				// "没有源"的负结果只缓存一小会儿:源被重新放回来时不必等 `.wmodel` 自己变化。
				double RetryProbeAt = 0.0;
			};

			struct ReimportRequest
			{
				uint64_t Serial = 0;
				std::filesystem::path ContentRoot;        // 派发时的内容根(提交时按它核对)
				std::string AssetLogical;                 // 目标 .wmodel 逻辑路径
				std::string SourceLogical;                // 源逻辑路径(meta 里的身份字段)
				std::string SourceAbsolute;
				Asset::ModelImportSettings Settings;      // 该 .wmodel 的导入设置(主线程读好)
				std::string SettingsWarning;              // 设置回退(读不出 meta)时的可读原因
				// 入队时目标 .wmodel 的 stat:提交前核对 —— 别人(面板 Reimport 等)已经改过
				// 这份资产时本产物已过期,丢弃而不是覆盖。
				std::uintmax_t BaselineSize = 0;
				std::filesystem::file_time_type BaselineMtime {};
				bool BaselineValid = false;
			};

			struct StagedFile
			{
				std::filesystem::path Temporary;
				std::filesystem::path Target;
			};

			struct ReimportOutcome
			{
				uint64_t Serial = 0;
				std::filesystem::path ContentRoot;
				std::string AssetLogical;
				std::string SourceLogical;
				bool Success = false;
				std::string Error;
				std::vector<StagedFile> Staged;   // 有序:贴图 → 材质 → 模型(.wmodel 最后)
				double ElapsedMs = 0.0;
				// 入队时目标 .wmodel 的 stat(提交前核对:别人改过就丢弃本产物)。
				std::uintmax_t BaselineSize = 0;
				std::filesystem::file_time_type BaselineMtime {};
				bool BaselineValid = false;
			};

			void Rescan();
			void UpdateFingerprint(const std::filesystem::path& sourceAbsolute, Fingerprint& state,
				double deltaSeconds, std::vector<std::filesystem::path>& outStableChanges);
			void EnqueueForSource(const std::filesystem::path& sourceAbsolute);
			// 单个资产的入队(设置/基线在主线程读好;被面板门挡住时只登记未决)。
			bool EnqueueReimport(const std::string& assetLogical, bool announceDeferral = true);
			bool EnqueueReimport(const WatchModel& model, bool announceDeferral = true);
			void RetryDeferred();
			void DropStagedFiles(const std::vector<StagedFile>& staged);
			void WorkerLoop();

			SkipProbe m_SkipProbe;
			std::filesystem::path m_ContentRoot;
			double m_RescanTimer = 0.0;
			double m_DeferredTimer = 0.0;
			std::vector<WatchModel> m_Models;
			// 去重后的源文件(多个 .wmodel 可以共享一个源;指纹按源文件存活)。
			std::vector<std::filesystem::path> m_Sources;
			std::unordered_map<std::string, ModelStamp> m_ModelStamps;
			// 上一次打印"model watch: N model(s)"时的计数(集合没变就不再打日志)。
			std::size_t m_ReportedModelCount = ~std::size_t { 0 };
			std::size_t m_ReportedSourceCount = ~std::size_t { 0 };
			// key = 绝对路径 generic 串(重扫会重建 m_Sources,指纹按文件路径存活)。
			std::unordered_map<std::string, Fingerprint> m_Fingerprints;
			// 面板打开被跳过、等面板关闭后重试的资产(唯一化,保持入队顺序)。
			std::vector<std::string> m_Deferred;

			std::thread m_Worker;
			std::mutex m_Mutex;
			std::condition_variable m_Cv;
			std::unordered_map<std::string, ReimportRequest> m_Pending;
			std::deque<std::string> m_PendingOrder;
			std::unordered_map<std::string, ReimportOutcome> m_Ready;
			std::deque<std::string> m_ReadyOrder;
			uint64_t m_Serial = 0;
			bool m_Shutdown = false;
		};
	}
}
