#include "wldpch.h"
#include "Asset/ModelImportWatch.h"

#include "World/Asset/GltfImporter.h"
#include "World/Asset/WModelIO.h"
#include "World/Core/Sha256.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Renderer/Mesh.h"
#include "World/Utils/Paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace World
{
	namespace
	{
		// 源扩展名(与 GltfImporter::Matches / 面板 ResolveSource 的候选顺序一致)。
		const char* const kSourceCandidates[] = { ".gltf", ".glb" };
		// "该 `.wmodel` 没有可监听的源"这一负结果的最长缓存时间(秒):超过就重读 meta/兜底探测,
		// 这样"源文件被重新放回来"不必等 `.wmodel` 自己变化也能恢复监听。
		constexpr double kNegativeSourceRetrySeconds = 30.0;

		double WallClockSeconds()
		{
			return std::chrono::duration<double>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		std::string LowerExtension(const std::filesystem::path& path)
		{
			std::string extension = path.extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return extension;
		}

		bool IsModelAssetFilePath(const std::filesystem::path& path)
		{
			return LowerExtension(path) == ".wmodel";
		}

		// 逻辑路径归一化:反斜杠 → 正斜杠;拒绝空/绝对/".."(内容根逃逸)。
		std::string NormalizeLogical(const std::string& logical)
		{
			std::string text = logical;
			std::replace(text.begin(), text.end(), '\\', '/');
			if (text.empty())
				return std::string();
			// 绝对路径 / 根相对路径 / 盘符:一律拒绝(内容根内只认相对逻辑路径)。
			if (text.front() == '/')
				return std::string();
			if (text.size() >= 2 && text[1] == ':')
				return std::string();
			const std::filesystem::path path(text);
			if (path.is_absolute())
				return std::string();
			for (const std::filesystem::path& part : path)
				if (part == "..")
					return std::string();
			// lexically_normal:把 "models/./x.gltf" 这类写法归一到同一条键
			// (按路径键存活的状态机不会被两种写法拆成两份)。
			return path.lexically_normal().generic_string();
		}

		bool IsRegularFile(const std::filesystem::path& path)
		{
			std::error_code error;
			return std::filesystem::is_regular_file(path, error);
		}

		bool FileStamp(const std::filesystem::path& path, std::uintmax_t& outSize,
			std::filesystem::file_time_type& outMtime)
		{
			std::error_code error;
			outSize = std::filesystem::file_size(path, error);
			if (error)
				return false;
			outMtime = std::filesystem::last_write_time(path, error);
			return !error;
		}

		bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input.is_open())
				return false;
			out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			return true;
		}

		// 同目录原子替换:临时文件与目标同卷,MoveFileEx(REPLACE_EXISTING) 是原子的
		// (与脚本编辑器/材质保存同一口径)。
		bool ReplaceFileAtomically(const std::filesystem::path& temporary,
			const std::filesystem::path& target, std::string& error)
		{
#ifdef _WIN32
			if (MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
				return true;
			error = "cannot replace " + target.generic_string()
				+ " (Win32 " + std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
			return false;
#else
			std::error_code renameError;
			std::filesystem::rename(temporary, target, renameError);
			if (renameError)
			{
				error = "cannot replace " + target.generic_string() + ": " + renameError.message();
				return false;
			}
			return true;
#endif
		}

		// 该 `.wmodel` 的导入源逻辑路径(与面板 ResolveSource 同一来源):
		//   ① meta.SourcePath(导入产物自报家门);② 旧产物按"同目录同名 .gltf/.glb"兜底。
		// 只有在源文件当前真实存在时才返回(源不存在 = 不监听)。
		std::string ResolveWatchedSource(const std::filesystem::path& contentRoot,
			const std::string& assetLogical)
		{
			const auto watchable = [&contentRoot](const std::string& logical)
			{
				const std::string normalized = NormalizeLogical(logical);
				if (normalized.empty())
					return std::string();
				return IsRegularFile(contentRoot / normalized) ? normalized : std::string();
			};

			World::Asset::WModelData::MetaData meta;
			std::string metaError;
			if (World::Asset::WModelIO::ReadMeta((contentRoot / assetLogical).string(), meta, &metaError)
				&& !meta.SourcePath.empty())
			{
				const std::string declared = watchable(meta.SourcePath);
				if (!declared.empty())
					return declared;
			}
			const std::filesystem::path asset(assetLogical);
			for (const char* extension : kSourceCandidates)
			{
				const std::filesystem::path candidate =
					asset.parent_path() / (asset.stem().string() + extension);
				const std::string found = watchable(candidate.generic_string());
				if (!found.empty())
					return found;
			}
			return std::string();
		}
	}

	namespace Editor
	{
		ModelImportWatch::~ModelImportWatch()
		{
			Shutdown();
		}

		void ModelImportWatch::Poll(double deltaSeconds)
		{
			const std::filesystem::path contentRoot = Paths::AssetRoot();
			if (contentRoot.empty())
				return;
			// 内容根变了(换项目/挂载点重建):清掉全部基线与未决,马上重扫。
			if (contentRoot != m_ContentRoot)
			{
				m_ContentRoot = contentRoot;
				m_Models.clear();
				m_Sources.clear();
				m_ModelStamps.clear();
				m_Fingerprints.clear();
				m_Deferred.clear();
				m_RescanTimer = 0.0;
			}
			m_RescanTimer -= deltaSeconds;
			if (m_RescanTimer <= 0.0)
			{
				m_RescanTimer = kRescanSeconds;
				Rescan();
				// 集合变化时才打一行(避免每 2s 刷屏)。
				if (m_Models.size() != m_ReportedModelCount || m_Sources.size() != m_ReportedSourceCount)
				{
					m_ReportedModelCount = m_Models.size();
					m_ReportedSourceCount = m_Sources.size();
					WLD_CORE_INFO("[asset-hot-reload] model watch: {0} model(s), {1} source(s) under '{2}'",
						m_Models.size(), m_Sources.size(), m_ContentRoot.generic_string());
				}
			}

			std::vector<std::filesystem::path> stableChanges;
			for (const std::filesystem::path& source : m_Sources)
			{
				std::unordered_map<std::string, Fingerprint>::iterator found =
					m_Fingerprints.find(source.generic_string());
				if (found == m_Fingerprints.end())
					continue;   // 不变量上到不了(重扫时会建);防御性跳过
				UpdateFingerprint(source, found->second, deltaSeconds, stableChanges);
			}
			for (const std::filesystem::path& source : stableChanges)
				EnqueueForSource(source);

			// 被面板门跳过、等面板关闭后重试的资产(节流,避免每帧访问壳状态)。
			m_DeferredTimer -= deltaSeconds;
			if (!m_Deferred.empty() && m_DeferredTimer <= 0.0)
			{
				m_DeferredTimer = kDeferredRetrySeconds;
				RetryDeferred();
			}
		}

		void ModelImportWatch::Rescan()
		{
			if (m_ContentRoot.empty() || !std::filesystem::is_directory(m_ContentRoot))
			{
				m_Models.clear();
				m_Sources.clear();
				m_ModelStamps.clear();
				m_Fingerprints.clear();
				return;
			}

			std::vector<std::filesystem::path> found;
			std::error_code walkError;
			std::filesystem::recursive_directory_iterator iterator(m_ContentRoot,
				std::filesystem::directory_options::skip_permission_denied, walkError);
			const std::filesystem::recursive_directory_iterator end;
			while (!walkError && iterator != end)
			{
				std::error_code entryError;
				const std::filesystem::directory_entry entry = *iterator;
				if (entry.is_regular_file(entryError) && IsModelAssetFilePath(entry.path()))
				{
					found.push_back(entry.path());
					if (found.size() >= kMaxWatchedModels)
						break;
				}
				iterator.increment(walkError);
			}
			std::sort(found.begin(), found.end());

			std::vector<WatchModel> models;
			std::vector<std::filesystem::path> sources;
			std::unordered_map<std::string, ModelStamp> stamps;
			for (const std::filesystem::path& absolute : found)
			{
				std::error_code relativeError;
				const std::filesystem::path relative =
					std::filesystem::relative(absolute, m_ContentRoot, relativeError);
				if (relativeError || relative.empty() || relative.is_absolute())
					continue;
				const std::string assetLogical = relative.generic_string();

				std::uintmax_t size = 0;
				std::filesystem::file_time_type mtime {};
				if (!FileStamp(absolute, size, mtime))
					continue;

				// stat 没变就不再读 meta(ReadMeta 会把整份 .wmodel 读进内存)。
				ModelStamp stamp;
				const auto cached = m_ModelStamps.find(assetLogical);
				// "没有源"的负结果只缓存 kNegativeSourceRetrySeconds;到期就重探一次。
				const bool negativeCached = cached != m_ModelStamps.end() && cached->second.Valid
					&& cached->second.SourceLogical.empty()
					&& WallClockSeconds() < cached->second.RetryProbeAt;
				if (cached != m_ModelStamps.end() && cached->second.Valid
					&& cached->second.Size == size && cached->second.Mtime == mtime
					&& (!cached->second.SourceLogical.empty() || negativeCached))
				{
					stamp = cached->second;
				}
				else
				{
					stamp.Valid = true;
					stamp.Size = size;
					stamp.Mtime = mtime;
					stamp.SourceLogical = ResolveWatchedSource(m_ContentRoot, assetLogical);
					stamp.RetryProbeAt = stamp.SourceLogical.empty()
						? WallClockSeconds() + kNegativeSourceRetrySeconds : 0.0;
				}
				stamps.emplace(assetLogical, stamp);
				if (stamp.SourceLogical.empty())
					continue;   // 从未导入 / 没有源 / 源不存在:忽略该源(T9 冲突门①)

				const std::filesystem::path sourceAbsolute = m_ContentRoot / stamp.SourceLogical;
				models.push_back(WatchModel { assetLogical, stamp.SourceLogical, sourceAbsolute });
				if (std::find(sources.begin(), sources.end(), sourceAbsolute) == sources.end())
					sources.push_back(sourceAbsolute);
			}

			m_Models.swap(models);
			m_Sources.swap(sources);
			m_ModelStamps.swap(stamps);
			// 不再被监听的源:清指纹(下次重新登记 = 建立新基线,不报告变化)。
			for (auto entry = m_Fingerprints.begin(); entry != m_Fingerprints.end(); )
			{
				const bool stillWatched = std::any_of(m_Sources.begin(), m_Sources.end(),
					[&entry](const std::filesystem::path& source)
					{ return source.generic_string() == entry->first; });
				entry = stillWatched ? std::next(entry) : m_Fingerprints.erase(entry);
			}
			for (const std::filesystem::path& source : m_Sources)
			{
				const std::string key = source.generic_string();
				if (m_Fingerprints.find(key) == m_Fingerprints.end())
					m_Fingerprints.emplace(key, Fingerprint {});
			}
			// 资产消失/换了源:未决重试跟着清掉。
			for (auto entry = m_Deferred.begin(); entry != m_Deferred.end(); )
			{
				const bool stillWatched = std::any_of(m_Models.begin(), m_Models.end(),
					[&entry](const WatchModel& model) { return model.AssetLogical == *entry; });
				entry = stillWatched ? std::next(entry) : m_Deferred.erase(entry);
			}
		}

		void ModelImportWatch::UpdateFingerprint(const std::filesystem::path& sourceAbsolute,
			Fingerprint& state, double deltaSeconds, std::vector<std::filesystem::path>& outStableChanges)
		{
			std::error_code statError;
			if (!std::filesystem::is_regular_file(sourceAbsolute, statError))
			{
				state = Fingerprint {};   // 文件消失:没有可导入的源;重新出现时按新基线处理
				return;
			}
			const std::uintmax_t size = std::filesystem::file_size(sourceAbsolute, statError);
			if (statError)
				return;
			const std::filesystem::file_time_type mtime =
				std::filesystem::last_write_time(sourceAbsolute, statError);
			if (statError)
				return;

			// ① 建立基线(首次登记,或刚被重新发现):记 stat + 内容哈希,**不**报告变化
			//    —— 与 AssetFileWatch::Watch 的"首次登记不产生变化"同一语义。
			if (!state.StampValid)
			{
				state.StampValid = true;
				state.Size = size;
				state.Mtime = mtime;
				state.Pending = false;
				state.PendingSeconds = 0.0;
				std::vector<uint8_t> bytes;
				if (ReadFileBytes(sourceAbsolute, bytes))
				{
					state.Hash = Crypto::Sha256Hex(bytes);
					state.HashKnown = true;
				}
				return;
			}
			// ② stat 没变、也没有未决变化 → 内容没变(便宜路径,不读盘)。
			if (!state.Pending && size == state.Size && mtime == state.Mtime)
				return;
			// ③ 未决窗口内 stat 也没变 → 只推进稳定计时(不重复读盘)。
			if (state.Pending && size == state.PendingSize && mtime == state.PendingMtime)
			{
				state.PendingSeconds += deltaSeconds;
			}
			else
			{
				// stat 变了:算内容哈希(**内容哈希优先**:同内容重写只动 mtime 不算变化)。
				std::vector<uint8_t> bytes;
				if (!ReadFileBytes(sourceAbsolute, bytes))
				{
					// 读不到(写者还没写完/权限):撤销未决,保持上次基线,下一帧再试。
					state.Pending = false;
					state.PendingSeconds = 0.0;
					return;
				}
				const std::string hash = Crypto::Sha256Hex(bytes);
				if (state.HashKnown && hash == state.Hash)
				{
					state.Size = size;
					state.Mtime = mtime;
					state.Pending = false;
					state.PendingSeconds = 0.0;
					return;
				}
				if (!state.Pending || state.PendingHash != hash)
					state.PendingSeconds = 0.0;   // 内容又变了 → 稳定窗口重启
				state.Pending = true;
				state.PendingHash = hash;
				state.PendingSize = size;
				state.PendingMtime = mtime;
			}

			if (!state.Pending)
				return;
			if (state.PendingSeconds < kStableSeconds)
				return;
			// ④ 稳定满窗口:接受为新基线并上报(重导入一次)。
			state.Hash = state.PendingHash;
			state.HashKnown = true;
			state.Size = state.PendingSize;
			state.Mtime = state.PendingMtime;
			state.Pending = false;
			state.PendingSeconds = 0.0;
			WLD_CORE_INFO("[asset-hot-reload] model changed '{0}' -> queued reimport",
				sourceAbsolute.generic_string());
			outStableChanges.push_back(sourceAbsolute);
		}

		void ModelImportWatch::EnqueueForSource(const std::filesystem::path& sourceAbsolute)
		{
			// 一个源可以产出多个 `.wmodel`(导入到不同目的地):逐个重导入。
			std::vector<std::string> targets;
			for (const WatchModel& model : m_Models)
			{
				if (model.SourceAbsolute == sourceAbsolute
					&& std::find(targets.begin(), targets.end(), model.AssetLogical) == targets.end())
					targets.push_back(model.AssetLogical);
			}
			for (const std::string& assetLogical : targets)
			{
				const auto model = std::find_if(m_Models.begin(), m_Models.end(),
					[&assetLogical](const WatchModel& candidate)
					{ return candidate.AssetLogical == assetLogical; });
				if (model != m_Models.end())
					EnqueueReimport(*model);
			}
		}

		bool ModelImportWatch::EnqueueReimport(const std::string& assetLogical, bool announceDeferral)
		{
			const auto model = std::find_if(m_Models.begin(), m_Models.end(),
				[&assetLogical](const WatchModel& candidate)
				{ return candidate.AssetLogical == assetLogical; });
			if (model == m_Models.end())
				return false;
			return EnqueueReimport(*model, announceDeferral);
		}

		bool ModelImportWatch::EnqueueReimport(const WatchModel& model, bool announceDeferral)
		{
			// 冲突门:面板正打开该资产 → 跳过(脏状态在面板私有状态里拿不到 ⇒ 打开即跳过),
			// 只登记未决、不丢:面板关闭后由 RetryDeferred 重试。
			if (m_SkipProbe && m_SkipProbe(model.AssetLogical))
			{
				if (std::find(m_Deferred.begin(), m_Deferred.end(), model.AssetLogical) == m_Deferred.end())
				{
					m_Deferred.push_back(model.AssetLogical);
					if (announceDeferral)
						WLD_CORE_INFO("[asset-hot-reload] model reimport deferred '{0}' -> {1}: "
							"model preview panel open (unsaved settings unknown)",
							model.SourceLogical, model.AssetLogical);
				}
				return false;
			}

			const std::filesystem::path assetAbsolute = m_ContentRoot / model.AssetLogical;
			std::uintmax_t baselineSize = 0;
			std::filesystem::file_time_type baselineMtime {};
			if (!FileStamp(assetAbsolute, baselineSize, baselineMtime))
			{
				WLD_CORE_INFO("[asset-hot-reload] model reimport skipped '{0}': asset missing",
					model.AssetLogical);
				return false;
			}

			ReimportRequest request;
			request.ContentRoot = m_ContentRoot;
			request.AssetLogical = model.AssetLogical;
			request.SourceLogical = model.SourceLogical;
			request.SourceAbsolute = model.SourceAbsolute.string();
			request.BaselineSize = baselineSize;
			request.BaselineMtime = baselineMtime;
			request.BaselineValid = true;
			// 设置的事实源 = 资产自身的 meta(与面板 Reimport 同一条解析入口);读不出来
			// 退回项目默认,原因记进警告(不失败)。
			request.Settings = Asset::ModelImportSettings::ResolveForImport(assetAbsolute.string(),
				&request.SettingsWarning, nullptr);
			if (!request.SettingsWarning.empty())
				WLD_CORE_WARN("[asset-hot-reload] model import settings for '{0}': {1}",
					model.AssetLogical, request.SettingsWarning);

			// 同一资产单飞:后来者覆盖未取走的请求。
			bool superseded = false;
			bool startedWorker = false;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_Shutdown)
					return false;
				request.Serial = ++m_Serial;
				const auto existing = m_Pending.find(model.AssetLogical);
				if (existing != m_Pending.end())
				{
					existing->second = std::move(request);
					superseded = true;
				}
				else
				{
					m_Pending.emplace(model.AssetLogical, std::move(request));
					m_PendingOrder.push_back(model.AssetLogical);
				}
				if (!m_Worker.joinable())
				{
					m_Worker = std::thread([this] { WorkerLoop(); });
					startedWorker = true;
				}
			}
			if (superseded)
				WLD_CORE_INFO("[asset-hot-reload] model watch: merged reimport for '{0}' (supersedes queued)",
					model.AssetLogical);
			if (startedWorker)
				WLD_CORE_INFO("[asset-hot-reload] model import worker started");
			m_Cv.notify_one();
			return true;
		}

		void ModelImportWatch::RetryDeferred()
		{
			if (m_Deferred.empty())
				return;
			std::vector<std::string> deferred;
			deferred.swap(m_Deferred);
			for (const std::string& assetLogical : deferred)
			{
				// 面板还没关 → EnqueueReimport 会重新登记;门已开 → 正常入队;
				// 资产/源没了 → 直接丢弃(重扫也会清掉未决)。重试不再重复打"deferred"日志。
				(void)EnqueueReimport(assetLogical, false);
			}
		}

		void ModelImportWatch::DropStagedFiles(const std::vector<StagedFile>& staged)
		{
			for (const StagedFile& file : staged)
			{
				std::error_code error;
				std::filesystem::remove(file.Temporary, error);
			}
		}

		void ModelImportWatch::Pump()
		{
			std::vector<ReimportOutcome> ready;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				while (!m_ReadyOrder.empty())
				{
					const std::string key = m_ReadyOrder.front();
					m_ReadyOrder.pop_front();
					const auto found = m_Ready.find(key);
					if (found == m_Ready.end())
						continue;
					ready.push_back(std::move(found->second));
					m_Ready.erase(found);
				}
			}

			// 提交(原子替换 + 清缓存 + 日志)只在主线程帧边界做,与面板 Reimport 同一收尾。
			for (ReimportOutcome& outcome : ready)
			{
				if (!outcome.Success || outcome.Staged.empty())
				{
					WLD_CORE_WARN("[asset-hot-reload] model reimport failed '{0}': {1}",
						outcome.SourceLogical, outcome.Error.empty() ? "no outputs" : outcome.Error);
					DropStagedFiles(outcome.Staged);
					continue;
				}
				// 内容根换过(换项目/关项目):这份产物已过期,丢弃。
				if (outcome.ContentRoot != Paths::AssetRoot())
				{
					WLD_CORE_INFO("[asset-hot-reload] model watch: dropped reimport for '{0}' "
						"(content root changed)", outcome.AssetLogical);
					DropStagedFiles(outcome.Staged);
					continue;
				}
				// 提交前再走一次冲突门:导入期间面板被打开 → 顺延到面板关闭之后。
				if (m_SkipProbe && m_SkipProbe(outcome.AssetLogical))
				{
					if (std::find(m_Deferred.begin(), m_Deferred.end(), outcome.AssetLogical) == m_Deferred.end())
						m_Deferred.push_back(outcome.AssetLogical);
					WLD_CORE_INFO("[asset-hot-reload] model reimport deferred '{0}' -> {1}: "
						"model preview panel open (unsaved settings unknown)",
						outcome.SourceLogical, outcome.AssetLogical);
					DropStagedFiles(outcome.Staged);
					continue;
				}
				// 基线核对:入队之后别人(面板 Reimport 等)已经改过这份资产 → 本产物过期,
				// 丢弃而不是覆盖(冲突门"只跳过、不覆盖")。
				std::uintmax_t currentSize = 0;
				std::filesystem::file_time_type currentMtime {};
				const bool stillMatches = outcome.BaselineValid
					&& FileStamp(outcome.ContentRoot / outcome.AssetLogical, currentSize, currentMtime)
					&& currentSize == outcome.BaselineSize && currentMtime == outcome.BaselineMtime;
				if (!stillMatches)
				{
					WLD_CORE_INFO("[asset-hot-reload] model watch: dropped stale reimport for '{0}' "
						"(asset changed while importing)", outcome.AssetLogical);
					DropStagedFiles(outcome.Staged);
					continue;
				}

				bool committed = true;
				std::string commitError;
				for (std::size_t index = 0; index < outcome.Staged.size(); ++index)
				{
					if (ReplaceFileAtomically(outcome.Staged[index].Temporary,
						outcome.Staged[index].Target, commitError))
						continue;
					committed = false;
					// 未替换的临时文件(含失败项)清掉;已替换的前几个保留(模型最后落盘,
					// 因此 `.wmodel` 仍保持旧内容 —— 与其他落盘路径同一残余风险口径)。
					std::vector<StagedFile> remaining(outcome.Staged.begin()
						+ static_cast<std::ptrdiff_t>(index), outcome.Staged.end());
					DropStagedFiles(remaining);
					break;
				}
				if (!committed)
				{
					WLD_CORE_WARN("[asset-hot-reload] model reimport failed '{0}': {1}",
						outcome.SourceLogical, commitError);
					continue;
				}
				// 与面板 Reimport 同一条收尾:清进程级网格/动画缓存,下一步读盘拿到新几何。
				Mesh::ClearWModelCache();
				AnimationSystem::ClearCache();
				m_Deferred.erase(std::remove(m_Deferred.begin(), m_Deferred.end(), outcome.AssetLogical),
					m_Deferred.end());
				WLD_CORE_INFO("[asset-hot-reload] model reimported '{0}' -> {1} ({2:.0f} ms)",
					outcome.SourceLogical, outcome.AssetLogical, outcome.ElapsedMs);
			}
		}

		void ModelImportWatch::Shutdown()
		{
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_Shutdown)
					return;
				m_Shutdown = true;
			}
			m_Cv.notify_all();
			// 等在飞的导入结束(工作线程只跑纯 CPU 导入 + 写临时文件,不碰 GPU/引擎缓存)。
			if (m_Worker.joinable())
				m_Worker.join();
			std::lock_guard<std::mutex> lock(m_Mutex);
			for (const auto& entry : m_Ready)
				for (const StagedFile& file : entry.second.Staged)
				{
					std::error_code error;
					std::filesystem::remove(file.Temporary, error);
				}
			m_Pending.clear();
			m_PendingOrder.clear();
			m_Ready.clear();
			m_ReadyOrder.clear();
		}

		void ModelImportWatch::WorkerLoop()
		{
			for (;;)
			{
				ReimportRequest request;
				{
					std::unique_lock<std::mutex> lock(m_Mutex);
					m_Cv.wait(lock, [this] { return m_Shutdown || !m_Pending.empty(); });
					if (m_Shutdown)
						return;
					if (m_PendingOrder.empty())
					{
						// 不变量上到不了(每次入队都压 Order);真出现就丢掉孤儿请求,避免空转。
						m_Pending.clear();
						continue;
					}
					const std::string frontKey = m_PendingOrder.front();
					m_PendingOrder.pop_front();
					const auto found = m_Pending.find(frontKey);
					if (found == m_Pending.end())
						continue;   // 防御性跳过,避免空转
					request = std::move(found->second);
					m_Pending.erase(found);
				}

				ReimportOutcome outcome;
				outcome.Serial = request.Serial;
				outcome.ContentRoot = request.ContentRoot;
				outcome.AssetLogical = request.AssetLogical;
				outcome.SourceLogical = request.SourceLogical;
				outcome.BaselineSize = request.BaselineSize;
				outcome.BaselineMtime = request.BaselineMtime;
				outcome.BaselineValid = request.BaselineValid;
				const double started = WallClockSeconds();
				try
				{
					// 与 cook / 编辑器导入同一条内核(CPU-only):设置/身份照 ImportFileImpl 填,
					// 目的地 = 该 `.wmodel` 现所在目录(刷新资产本身,不新开一份)。
					Asset::GltfImportMetadata metadata;
					metadata.ImporterVersion = 1;
					metadata.SettingsHash = Asset::ModelImportSettings::Hash(request.Settings);
					metadata.UpAxis = request.Settings.UpAxis;
					metadata.Scale = request.Settings.Scale;
					metadata.LogicalModelPath.clear();
					metadata.DestinationLogicalDir =
						std::filesystem::path(request.AssetLogical).parent_path().generic_string();
					metadata.ContentRootAbsolute = request.ContentRoot.string();
					metadata.SourceLogicalPath = request.SourceLogical;

					Asset::GltfImportBytesResult bytes;
					std::string importError;
					if (!Asset::GltfImporter::ImportAsBytes(request.SourceAbsolute, request.Settings,
						metadata, &bytes, &importError))
					{
						outcome.Error = importError.empty() ? "glTF import failed" : importError;
					}
					else
					{
						const std::string produced = NormalizeLogical(bytes.Summary.WModelPath);
						if (produced != request.AssetLogical)
						{
							outcome.Error = "import produced '" + produced + "', expected '"
								+ request.AssetLogical + "' (asset was renamed after import)";
						}
						else
						{
							outcome.Success = true;
							std::string stageError;
							for (const Asset::GltfInMemoryOutput& output : bytes.Outputs)
							{
								const std::string logical = NormalizeLogical(output.LogicalPath);
								if (logical.empty())
								{
									stageError = "unsafe output path '" + output.LogicalPath + "'";
									break;
								}
								StagedFile staged;
								staged.Target = request.ContentRoot / logical;
								staged.Temporary = staged.Target;
								staged.Temporary += ".tmp-hotreload-" + std::to_string(request.Serial);
								std::error_code directoryError;
								if (!staged.Target.parent_path().empty())
									std::filesystem::create_directories(staged.Target.parent_path(), directoryError);
								std::ofstream stream(staged.Temporary, std::ios::binary | std::ios::trunc);
								if (!stream.is_open())
								{
									stageError = "cannot create temporary file '" + logical + "'";
								}
								else
								{
									stream.write(reinterpret_cast<const char*>(output.Data.data()),
										static_cast<std::streamsize>(output.Data.size()));
									stream.flush();
									if (!stream.good())
										stageError = "cannot write temporary file '" + logical + "'";
								}
								stream.close();
								if (!stageError.empty())
									break;
								outcome.Staged.push_back(std::move(staged));
							}
							if (!stageError.empty())
							{
								outcome.Success = false;
								outcome.Error = stageError;
							}
						}
					}
				}
				catch (const std::exception& exception)
				{
					outcome.Success = false;
					outcome.Error = std::string("import threw: ") + exception.what();
				}
				catch (...)
				{
					outcome.Success = false;
					outcome.Error = "import threw an unknown exception";
				}
				outcome.ElapsedMs = (WallClockSeconds() - started) * 1000.0;
				if (!outcome.Success && !outcome.Staged.empty())
					DropStagedFiles(outcome.Staged);

				bool dropped = false;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					if (m_Shutdown)
					{
						std::error_code error;
						for (const StagedFile& file : outcome.Staged)
							std::filesystem::remove(file.Temporary, error);
						return;
					}
					// 后来者胜:导入期间同资产又被入队 → 这份产物已经过期,直接丢。
					if (m_Pending.find(request.AssetLogical) != m_Pending.end())
						dropped = true;
					else
					{
						const std::string& key = request.AssetLogical;
						const auto ready = m_Ready.find(key);
						if (ready == m_Ready.end())
						{
							m_Ready.emplace(key, std::move(outcome));
							m_ReadyOrder.push_back(key);
						}
						else
						{
							// 覆盖尚未被 Pump 消费的旧产物:先清掉它已写好的临时文件(防泄漏)。
							for (const StagedFile& file : ready->second.Staged)
							{
								std::error_code cleanupError;
								std::filesystem::remove(file.Temporary, cleanupError);
							}
							ready->second = std::move(outcome);   // 覆盖尚未被 Pump 消费的旧产物
						}
					}
				}
				if (dropped)
				{
					std::error_code error;
					for (const StagedFile& file : outcome.Staged)
						std::filesystem::remove(file.Temporary, error);
					WLD_CORE_INFO("[asset-hot-reload] model watch: dropped superseded reimport for '{0}'",
						request.AssetLogical);
				}
			}
		}
	}
}
