#pragma once

#include "World/Renderer/Texture/TextureArtifact.h"
#include "World/Renderer/Texture/TextureImportSettings.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace World
{
	namespace Editor
	{
		// HOTR-P2-T5(P2-c):内容根下纹理资产(`.wtex`)与它们的 `source:` 源图被**外部改动**时,
		// 生效矩阵与开关:`docs/dev/hot-reload.md`。
		// 编辑器自动重烘 `<同目录>/<主名>.wtexc` 并失效材质贴图缓存。
		//
		// 动机:M4-TEX 的导入管线只在"用户点 Apply/Reimport"或 cook 时重烘;`.wtex`(或其源图)
		// 被外部工具/另一个编辑器改掉后,运行中编辑器仍读旧产物 —— 只能手动 Reimport。
		//
		// 口径(与 `AssetFileWatch` / 面板的预览烘焙同一套,不新造线程模型):
		//   * **内容哈希优先**:同内容重写(只动 mtime)不算变化;stat 先行,stat 没变不读盘;
		//   * **稳定窗口**:变化内容连续稳定 kStableSeconds 秒后才入队(期间再变则重启计时);
		//   * **2s 重扫**:发现新增 / 被删除的 `.wtex`(首次登记 = 建立基线,不重烘);
		//   * **主线程不做重编码**:编码在工作线程(`TextureCompiler::BakeBytes/BakeFile`,纯 CPU);
		//     写盘 + `MaterialTextureCache::Invalidate` + 日志只在主线程帧边界的 Pump 里做;
		//   * **绝不写 `.wtex`**:本服务只写派生产物(`.wtexc`),用户/面板的未保存编辑永远不被覆盖
		//     (面板的 SyncWithDisk 仍按它自己的口径给出"磁盘已变、未覆盖"的提示)。
		//
		// 只读发现:重扫时只读 `.wtex` 的头部文本(最多 kHeaderProbeBytes,避免把内嵌 payload
		// 全读进内存);权威读取仍是 Editor::LoadTextureAssetDocument / ResolveTextureSource。
		class TextureImportWatch
		{
		public:
			TextureImportWatch() = default;
			~TextureImportWatch();

			TextureImportWatch(const TextureImportWatch&) = delete;
			TextureImportWatch& operator=(const TextureImportWatch&) = delete;

			// 主线程,帧边界(资产热重载的同一道开关内):重扫 + 指纹轮询 + 稳定变化入队。
			void Poll(double deltaSeconds);
			// 主线程,帧边界:取回工作线程烘好的产物 → 契约名写盘 + 材质贴图缓存失效 + 日志。
			void Pump();
			// 停止并 join 工作线程(幂等;OnDetach 与析构都会调)。
			void Shutdown();

			// 重扫内容根(发现新增 / 消失的 `.wtex`)的周期(秒)。
			static constexpr double kRescanSeconds = 2.0;
			// 变化内容必须稳定满这个秒数才入队(与 AssetFileWatch 的"连续稳定 debounce"同一口径)。
			static constexpr double kStableSeconds = 2.0;
			// 头探测上限:超过它就当"非容器"处理(权威读取仍会读到真实内容)。
			static constexpr std::size_t kHeaderProbeBytes = 256 * 1024;
			// 防御上限:一次重扫最多登记多少个 `.wtex`(异常目录不拖爆每帧开销)。
			static constexpr std::size_t kMaxWatchedAssets = 4096;

			// 诊断(主线程读;日志/报告用)。
			std::size_t WatchedAssetCount() const { return m_Assets.size(); }
			std::size_t WatchedFileCount() const { return m_Files.size(); }

		private:
			// 一个被监听文件的指纹状态机(stat 先行 + 内容哈希 + 稳定窗口)。
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

			struct WatchFile
			{
				std::string AssetLogical;     // 主键 = 资产的逻辑路径(重烘与日志用)
				std::filesystem::path Absolute;
			};

			struct BakeRequest
			{
				uint64_t Serial = 0;
				std::filesystem::path ContentRoot;       // 派发时的内容根(提交时按它落盘)
				std::string AssetLogical;
				std::string SourceLogical;              // 字节来源逻辑路径(容器 = 资产自身)
				std::string AbsoluteSource;             // 旧式:外部源图绝对路径(容器时为空)
				std::vector<uint8_t> Bytes;             // 容器:内嵌 payload(主线程读好)
				TextureImportSettings Settings;
			};

			struct BakeOutcome
			{
				uint64_t Serial = 0;
				std::filesystem::path ContentRoot;
				std::string AssetLogical;
				std::string SourceLogical;
				bool Success = false;
				std::string Error;
				std::vector<uint8_t> Bytes;
				TextureArtifactHeader Header;
				double ElapsedMs = 0.0;
			};

			void Rescan();
			void UpdateFingerprint(const WatchFile& file, Fingerprint& state, double deltaSeconds,
				std::vector<std::string>& outStableChanges);
			void EnqueueBake(const std::string& assetLogical);
			void WorkerLoop();

			std::filesystem::path m_ContentRoot;
			double m_RescanTimer = 0.0;
			std::vector<WatchFile> m_Files;
			// 上一次打印"texture watch: N asset(s)"时的计数(集合没变就不再打日志)。
			std::size_t m_ReportedAssetCount = ~std::size_t { 0 };
			std::size_t m_ReportedFileCount = ~std::size_t { 0 };
			// key = 绝对路径 generic 串(重扫会重建 m_Files,指纹按文件路径存活)。
			std::unordered_map<std::string, Fingerprint> m_Fingerprints;
			// 已登记资产的逻辑路径(重扫时用来判断新增/消失)。
			std::vector<std::string> m_Assets;

			std::thread m_Worker;
			std::mutex m_Mutex;
			std::condition_variable m_Cv;
			std::unordered_map<std::string, BakeRequest> m_Pending;
			std::deque<std::string> m_PendingOrder;
			std::unordered_map<std::string, BakeOutcome> m_Ready;
			std::deque<std::string> m_ReadyOrder;
			uint64_t m_Serial = 0;
			bool m_Shutdown = false;
		};
	}
}
