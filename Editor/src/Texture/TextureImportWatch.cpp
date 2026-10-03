#include "wldpch.h"
#include "Texture/TextureImportWatch.h"

#include "Texture/TextureArtifactBaker.h"

// 资产读取 / 源解析的**唯一口径**(容器 / 旧式自动区分)在面板模块里,本文件只调用。
#include "WUI/Panels/TextureSettingsPanel.h"

#include "World/Core/Sha256.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Texture/TextureCompiler.h"
#include "World/Utils/Paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace World
{
	namespace
	{
		// 源图扩展名(与 TextureCompiler::BakeDirectory / 面板的候选顺序一致)。
		const char* const kSourceCandidates[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
		constexpr std::size_t kReadChunkBytes = 8192;
		// 头探测上限(公开常量在 TextureImportWatch 里;匿名命名空间里取一份便于内联使用)。
		constexpr std::size_t kHeaderProbeLimit = Editor::TextureImportWatch::kHeaderProbeBytes;

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

		bool IsTextureAssetFilePath(const std::filesystem::path& path)
		{
			return LowerExtension(path) == ".wtex";
		}

		std::string TrimAscii(const std::string& text)
		{
			const auto notSpace = [](unsigned char character) { return std::isspace(character) == 0; };
			std::string trimmed = text;
			trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), notSpace));
			trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), notSpace).base(), trimmed.end());
			return trimmed;
		}

		bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out,
			std::string& error)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input.is_open())
			{
				error = "cannot read " + path.generic_string();
				return false;
			}
			out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			return true;
		}

		// 只读 `.wtex` 的**头部文本**(最多 kHeaderProbeBytes):给出"是否单文件容器"与 `source:` 值。
		// 目的:2s 重扫时不必把内嵌 payload(可能几 MB)读进内存;权威读取仍是
		// `Editor::LoadTextureAssetDocument` / `ResolveTextureSource`(只在真要重烘时跑一次)。
		bool ProbeTextureAssetHeader(const std::filesystem::path& file, bool& outContainer,
			std::string& outSource)
		{
			outContainer = false;
			outSource.clear();
			std::ifstream input(file, std::ios::binary);
			if (!input.is_open())
				return false;
			std::string text;
			text.reserve(kReadChunkBytes);
			char buffer[kReadChunkBytes];
			while (text.size() < kHeaderProbeLimit && input.good())
			{
				input.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
				const std::streamsize read = input.gcount();
				if (read <= 0)
					break;
				text.append(buffer, static_cast<std::size_t>(read));
				const std::size_t marker = text.find(kTextureAssetPayloadMarker);
				if (marker != std::string::npos)
				{
					text.erase(marker);
					outContainer = true;
					break;
				}
			}
			std::istringstream stream(text);
			std::string line;
			while (std::getline(stream, line))
			{
				const std::string trimmed = TrimAscii(line);
				if (trimmed.rfind("source:", 0) != 0)
					continue;
				std::string value = TrimAscii(trimmed.substr(std::string("source:").size()));
				if (value.size() >= 2
					&& ((value.front() == '"' && value.back() == '"')
						|| (value.front() == '\'' && value.back() == '\'')))
					value = value.substr(1, value.size() - 2);
				outSource = value;
				break;
			}
			return true;
		}
	}

	namespace Editor
	{
		TextureImportWatch::~TextureImportWatch()
		{
			Shutdown();
		}

		void TextureImportWatch::Poll(double deltaSeconds)
		{
			const std::filesystem::path contentRoot = Paths::AssetRoot();
			if (contentRoot.empty())
				return;
			// 内容根变了(换项目/挂载点重建):清掉全部基线与待烘,马上重扫。
			if (contentRoot != m_ContentRoot)
			{
				m_ContentRoot = contentRoot;
				m_Files.clear();
				m_Assets.clear();
				m_Fingerprints.clear();
				m_RescanTimer = 0.0;
			}
			m_RescanTimer -= deltaSeconds;
			if (m_RescanTimer <= 0.0)
			{
				m_RescanTimer = kRescanSeconds;
				Rescan();
				// 集合变化时才打一行(避免每 2s 刷屏)。
				if (m_Assets.size() != m_ReportedAssetCount || m_Files.size() != m_ReportedFileCount)
				{
					m_ReportedAssetCount = m_Assets.size();
					m_ReportedFileCount = m_Files.size();
					WLD_CORE_INFO("[asset-hot-reload] texture watch: {0} asset(s), {1} file(s) under '{2}'",
						m_Assets.size(), m_Files.size(), m_ContentRoot.generic_string());
				}
			}

			std::vector<std::string> stableChanges;
			for (const WatchFile& file : m_Files)
			{
				std::unordered_map<std::string, Fingerprint>::iterator found =
					m_Fingerprints.find(file.Absolute.generic_string());
				if (found == m_Fingerprints.end())
					continue;   // 不变量上到不了(重扫时会建);防御性跳过
				UpdateFingerprint(file, found->second, deltaSeconds, stableChanges);
			}
			for (const std::string& assetLogical : stableChanges)
				EnqueueBake(assetLogical);
		}

		void TextureImportWatch::Rescan()
		{
			if (m_ContentRoot.empty() || !std::filesystem::is_directory(m_ContentRoot))
			{
				m_Files.clear();
				m_Assets.clear();
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
				if (entry.is_regular_file(entryError) && IsTextureAssetFilePath(entry.path()))
				{
					found.push_back(entry.path());
					if (found.size() >= kMaxWatchedAssets)
						break;
				}
				iterator.increment(walkError);
			}
			std::sort(found.begin(), found.end());

			std::vector<WatchFile> files;
			std::vector<std::string> assets;
			std::unordered_map<std::string, Fingerprint> next;
			const auto ensureFingerprint = [this, &next](const std::filesystem::path& absolute)
			{
				const std::string key = absolute.generic_string();
				if (next.find(key) != next.end())
					return;
				const auto existing = m_Fingerprints.find(key);
				next.emplace(key, existing != m_Fingerprints.end() ? existing->second
					: Fingerprint {});
			};

			for (const std::filesystem::path& absolute : found)
			{
				std::error_code relativeError;
				const std::filesystem::path relative =
					std::filesystem::relative(absolute, m_ContentRoot, relativeError);
				if (relativeError || relative.empty() || relative.is_absolute())
					continue;
				const std::string logical = relative.generic_string();
				assets.push_back(logical);
				files.push_back(WatchFile { logical, absolute });
				ensureFingerprint(absolute);

				// 旧式设置文件(没有 `---payload`):字节来源是外部源图,源图变化也要重烘。
				// 容器形态的字节内嵌在 `.wtex` 里,监听 `.wtex` 本身就够了。
				bool container = false;
				std::string declaredSource;
				if (!ProbeTextureAssetHeader(absolute, container, declaredSource) || container)
					continue;
				std::string sourceLogical = declaredSource;
				if (sourceLogical.empty())
				{
					const std::filesystem::path asset(logical);
					for (const char* extension : kSourceCandidates)
					{
						const std::filesystem::path candidate =
							asset.parent_path() / (asset.stem().string() + extension);
						std::error_code existsError;
						if (std::filesystem::is_regular_file(m_ContentRoot / candidate, existsError))
						{
							sourceLogical = candidate.generic_string();
							break;
						}
					}
				}
				if (sourceLogical.empty())
					continue;
				const std::filesystem::path sourceAbsolute = m_ContentRoot / sourceLogical;
				std::error_code existsError;
				if (!std::filesystem::is_regular_file(sourceAbsolute, existsError))
					continue;
				files.push_back(WatchFile { logical, sourceAbsolute });
				ensureFingerprint(sourceAbsolute);
			}

			m_Fingerprints.swap(next);
			m_Files.swap(files);
			m_Assets.swap(assets);
		}

		void TextureImportWatch::UpdateFingerprint(const WatchFile& file, Fingerprint& state,
			double deltaSeconds, std::vector<std::string>& outStableChanges)
		{
			std::error_code statError;
			if (!std::filesystem::is_regular_file(file.Absolute, statError))
			{
				state = Fingerprint {};   // 文件消失:没有可烘的内容;重新出现时按新基线处理
				return;
			}
			const std::uintmax_t size = std::filesystem::file_size(file.Absolute, statError);
			if (statError)
				return;
			const std::filesystem::file_time_type mtime =
				std::filesystem::last_write_time(file.Absolute, statError);
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
				std::string readError;
				if (ReadFileBytes(file.Absolute, bytes, readError))
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
				std::string readError;
				if (!ReadFileBytes(file.Absolute, bytes, readError))
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
			// ④ 稳定满窗口:接受为新基线并上报(重烘一次)。
			state.Hash = state.PendingHash;
			state.HashKnown = true;
			state.Size = state.PendingSize;
			state.Mtime = state.PendingMtime;
			state.Pending = false;
			state.PendingSeconds = 0.0;
			WLD_CORE_INFO("[asset-hot-reload] texture changed '{0}' -> queued rebake", file.AssetLogical);
			outStableChanges.push_back(file.AssetLogical);
		}

		void TextureImportWatch::EnqueueBake(const std::string& assetLogical)
		{
			// 主线程:把"这一版磁盘内容"快照进请求(容器 = 内嵌 payload;旧式 = 外部源图路径)。
			// 权威读取口径与面板一致(`LoadTextureAssetDocument` + `ResolveTextureSource`)。
			const TextureAssetDocument document = LoadTextureAssetDocument(m_ContentRoot, assetLogical);
			if (!document.Valid)
			{
				WLD_CORE_WARN("[asset-hot-reload] texture rebake skipped '{0}': {1}", assetLogical,
					document.Error);
				return;
			}
			TextureSourceResolution resolution;
			if (!ResolveTextureSource(m_ContentRoot, assetLogical, document.Settings, resolution))
			{
				WLD_CORE_WARN("[asset-hot-reload] texture rebake skipped '{0}': {1}", assetLogical,
					resolution.Error);
				return;
			}

			BakeRequest request;
			request.ContentRoot = m_ContentRoot;
			request.AssetLogical = assetLogical;
			request.SourceLogical = resolution.BytesLogical;
			request.Settings = document.Settings;
			// 字节来源(与面板的 CurrentSourceBytes 同口径):容器 = 内嵌 payload;旧式 = 外部源图文件。
			if (resolution.Embedded && !document.Payload.empty())
				request.Bytes = document.Payload;
			else if (!resolution.BytesLogical.empty() && !resolution.Embedded)
				request.AbsoluteSource = (m_ContentRoot / resolution.BytesLogical).string();
			else
			{
				WLD_CORE_WARN("[asset-hot-reload] texture rebake skipped '{0}': "
					"the byte source is empty (container without payload)", assetLogical);
				return;
			}

			bool superseded = false;
			bool startedWorker = false;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_Shutdown)
					return;
				request.Serial = ++m_Serial;
				const auto existing = m_Pending.find(assetLogical);
				if (existing != m_Pending.end())
				{
					existing->second = std::move(request);   // 同资产单飞:后来者覆盖
					superseded = true;
				}
				else
				{
					m_Pending.emplace(assetLogical, std::move(request));
					m_PendingOrder.push_back(assetLogical);
				}
				if (!m_Worker.joinable())
				{
					m_Worker = std::thread([this] { WorkerLoop(); });
					startedWorker = true;
				}
			}
			if (superseded)
				WLD_CORE_INFO("[asset-hot-reload] texture watch: merged rebake for '{0}' (supersedes queued)",
					assetLogical);
			if (startedWorker)
				WLD_CORE_INFO("[asset-hot-reload] texture bake worker started");
			m_Cv.notify_one();
		}

		void TextureImportWatch::Pump()
		{
			std::vector<BakeOutcome> ready;
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
			// 写盘 + 缓存失效只在主线程做(与面板 Apply 同一条提交实现)。
			for (const BakeOutcome& outcome : ready)
			{
				if (!outcome.Success)
				{
					WLD_CORE_WARN("[asset-hot-reload] texture rebake failed '{0}': {1}",
						outcome.AssetLogical, outcome.Error);
					continue;
				}
				std::string commitError;
				const std::filesystem::path byteSourceFile = outcome.ContentRoot / outcome.SourceLogical;
				if (!CommitTextureArtifact(outcome.ContentRoot, outcome.SourceLogical, byteSourceFile,
						outcome.Bytes, outcome.Header, commitError))
				{
					WLD_CORE_WARN("[asset-hot-reload] texture rebake failed '{0}': {1}",
						outcome.AssetLogical, commitError);
					continue;
				}
				WLD_CORE_INFO("[asset-hot-reload] texture rebaked '{0}' ({1:.0f} ms)",
					outcome.AssetLogical, outcome.ElapsedMs);
				// 重烘产物落地后还要让**引用该贴图的材质**失效(Revision 前进):
				// 否则渲染侧仍拿旧描述符集(实测:第二次重烘后画面停在上一轮颜色)。
				MaterialLibrary::Get().InvalidateTextureDependents(outcome.AssetLogical);
			}
		}

		void TextureImportWatch::Shutdown()
		{
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_Shutdown)
					return;
				m_Shutdown = true;
			}
			m_Cv.notify_all();
			// 等在飞的编码结束(工作线程只跑纯 CPU 烘焙,不碰 GPU/文件写)。
			if (m_Worker.joinable())
				m_Worker.join();
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Pending.clear();
			m_PendingOrder.clear();
			m_Ready.clear();
			m_ReadyOrder.clear();
		}

		void TextureImportWatch::WorkerLoop()
		{
			for (;;)
			{
				BakeRequest request;
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

				BakeOutcome outcome;
				outcome.Serial = request.Serial;
				outcome.ContentRoot = request.ContentRoot;
				outcome.AssetLogical = request.AssetLogical;
				outcome.SourceLogical = request.SourceLogical;
				const double started = WallClockSeconds();
				try
				{
					// 工作线程只跑纯 CPU 烘焙(与面板的预览烘焙同一调用):容器按内嵌字节,旧式按外部源图。
					outcome.Success = !request.Bytes.empty()
						? TextureCompiler::BakeBytes(request.Bytes, request.Settings, outcome.Bytes,
							outcome.Header, outcome.Error)
						: TextureCompiler::BakeFile(std::filesystem::path(request.AbsoluteSource),
							request.Settings, outcome.Bytes, outcome.Header, outcome.Error);
				}
				catch (const std::exception& exception)
				{
					outcome.Success = false;
					outcome.Error = std::string("bake threw: ") + exception.what();
				}
				catch (...)
				{
					outcome.Success = false;
					outcome.Error = "bake threw an unknown exception";
				}
				outcome.ElapsedMs = (WallClockSeconds() - started) * 1000.0;

				bool dropped = false;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					if (m_Shutdown)
						return;
					// 后来者胜:编码期间同资产又被入队 → 这份产物已经过期,直接丢。
					if (m_Pending.find(request.AssetLogical) != m_Pending.end())
					{
						dropped = true;
					}
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
							ready->second = std::move(outcome);   // 覆盖尚未被 Pump 消费的旧产物
					}
				}
				if (dropped)
					WLD_CORE_INFO("[asset-hot-reload] texture watch: dropped superseded rebake for '{0}'",
						request.AssetLogical);
			}
		}
	}
}
