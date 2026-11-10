#include "Integrations/ShaderHotReload.h"

#include "World/Core/Log.h"
#include "World/Renderer/AssetHotReload.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/MaterialSurfaceRuntime.h"
#include "World/Renderer/Renderer.h"
#include "World/Utils/Paths.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <utility>

namespace World::Editor
{
	namespace
	{
		// 与 MaterialEditorPanel::ShaderBackendIsVulkan 同一口径(大小写不敏感、含 "vulkan")。
		// GPU 目标必须与当前设备一致 —— MaterialSurfaceRuntime::Install 也按设备后端校验产物。
		bool BackendIsVulkan()
		{
			std::string name = Renderer::GetBackendName();
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return name.find("vulkan") != std::string::npos;
		}

		// 库文件(`shaders/lib/**`)不是材质表面:没有 `Evaluate` 入口、不单独编译 ——
		// 判定与 MaterialEditorPanel::IsMaterialLibraryShaderPath 逐条一致(扩展名 + 路径片段)。
		// MAT-FN3 起"库文件改了"由引用它的**根材质着色器路径**上报(T2),所以这里只是一道防线:
		// 万一有材质把 Shader 直接写成 lib 文件,也不会把它当表面去编译。
		bool IsLibraryShaderPath(const std::string& normalized)
		{
			std::string lowered = normalized;
			std::replace(lowered.begin(), lowered.end(), '\\', '/');
			std::transform(lowered.begin(), lowered.end(), lowered.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			const std::string extension = ".slang";
			if (lowered.size() <= extension.size()
				|| lowered.compare(lowered.size() - extension.size(), extension.size(), extension) != 0)
				return false;
			const std::string marker = "shaders/lib/";
			std::size_t at = lowered.find(marker);
			while (at != std::string::npos)
			{
				if (at == 0 || lowered[at - 1] == '/')
					return true;
				at = lowered.find(marker, at + 1);
			}
			return false;
		}

		// 表面材质编译器的 include 根(绝对路径)。顺序 = docs/dev/shader-contract.md §9:
		// **先材质自身目录、再内容根 `shaders/`**。只收集真实存在的目录 —— 未落盘的新材质
		// 推出来的目录可能不存在(这时命中的只能是内容根,和"没写这个根"等价);顺序固定,
		// 根本身不进缓存键。语义照 MaterialEditorPanel 的 SurfaceIncludeRoots 实现(唯一副本
		// 在那边;本文件不共用面板私有函数)。
		std::vector<std::filesystem::path> SurfaceIncludeRoots(const std::filesystem::path& contentRoot,
			const std::string& shaderLogicalPath)
		{
			std::vector<std::filesystem::path> roots;
			const auto add = [&roots](const std::filesystem::path& candidate)
			{
				if (candidate.empty())
					return;
				std::error_code error;
				if (!std::filesystem::is_directory(candidate, error))
					return;
				const std::filesystem::path absolute = std::filesystem::absolute(candidate, error);
				if (error || absolute.empty())
					return;
				const std::filesystem::path normalized = absolute.lexically_normal();
				if (std::find(roots.begin(), roots.end(), normalized) == roots.end())
					roots.push_back(normalized);
			};
			// 材质自身目录:未落盘时按逻辑路径推(绝对路径直接用)。
			const std::filesystem::path shaderFile(shaderLogicalPath);
			add((shaderFile.is_absolute() ? shaderFile : contentRoot / shaderFile).parent_path());
			add(contentRoot / "shaders");
			return roots;
		}

		// 失败原因 = 第一条 error 诊断(用户源错误带行:列);没有 error 时退第一条诊断,
		// 再退工具输出的第一行。口径与面板状态行的"first error"一致,只是不做本地化。
		std::string FirstDiagnosticText(const SurfaceCompileResult& result)
		{
			const SurfaceDiagnostic* chosen = nullptr;
			for (const SurfaceDiagnostic& diagnostic : result.Diagnostics)
			{
				if (diagnostic.Severity == "error")
				{
					chosen = &diagnostic;
					break;
				}
			}
			if (!chosen && !result.Diagnostics.empty())
				chosen = &result.Diagnostics.front();
			if (chosen)
			{
				std::string text = chosen->Severity.empty() ? std::string("error") : chosen->Severity;
				text += ": ";
				if (chosen->InUserSource && chosen->Line > 0)
				{
					char position[64] = {};
					std::snprintf(position, sizeof(position), "line %u:%u  ",
						static_cast<unsigned>(chosen->Line), static_cast<unsigned>(chosen->Column));
					text += position;
				}
				text += chosen->Message;
				return text;
			}
			std::string raw = result.RawToolOutput;
			// 日志一行一条:原始输出可能多行,只留第一行。
			const std::size_t newline = raw.find_first_of("\r\n");
			if (newline != std::string::npos)
				raw.erase(newline);
			if (!raw.empty())
				return raw;
			return "the compiler returned no diagnostics";
		}
	}

	ShaderHotReload::~ShaderHotReload()
	{
		Shutdown();
	}

	void ShaderHotReload::Enqueue(const std::string& logicalPath)
	{
		const std::string key = MaterialLibrary::NormalizePath(logicalPath);
		if (key.empty())
			return;
		if (IsLibraryShaderPath(key))
		{
			WLD_CORE_INFO("[shader-hot-reload] skipped key='{0}' (material library file, not a surface)", key);
			return;
		}

		// 后端与 include 根在**主线程**取(工作线程只读请求副本;面板的后台编译同一口径)。
		CompileRequest request;
		request.LogicalPath = key;
		request.Backend = BackendIsVulkan()
			? SurfaceShaderBackend::VulkanSpirV : SurfaceShaderBackend::OpenGLSpirV;
		request.IncludeRoots = SurfaceIncludeRoots(Paths::AssetRoot(), key);
		// 源文本同样在主线程读:与面板一致(工作线程只跑编译器);读不到 = 一次失败日志,
		// 保留旧管线,不投递空请求。
		{
			std::vector<uint8_t> bytes;
			std::string readError;
			if (!ReadAssetBytes(key, bytes, &readError))
			{
				WLD_CORE_WARN("[shader-hot-reload] compile failed key='{0}': {1}", key,
					readError.empty() ? std::string("cannot read the shader source") : readError);
				return;
			}
			request.Source.assign(bytes.begin(), bytes.end());
		}

		bool superseded = false;
		bool startedWorker = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_Shutdown)
				return;
			request.Serial = ++m_Serial;
			const auto existing = m_Pending.find(key);
			if (existing != m_Pending.end())
			{
				existing->second = std::move(request);   // 同路径单飞:后来者覆盖
				superseded = true;
			}
			else
			{
				m_Pending.emplace(key, std::move(request));
				m_PendingOrder.push_back(key);
			}
			if (!m_Worker.joinable())
			{
				m_Worker = std::thread([this] { WorkerLoop(); });
				startedWorker = true;
			}
		}
		if (superseded)
			WLD_CORE_INFO("[shader-hot-reload] merged key='{0}' (supersedes the queued request)", key);
		else
			WLD_CORE_INFO("[shader-hot-reload] queued key='{0}'", key);
		if (startedWorker)
			WLD_CORE_INFO("[shader-hot-reload] compile worker started");
		m_Cv.notify_one();
	}

	void ShaderHotReload::Pump()
	{
		std::vector<CompileOutcome> ready;
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
		// Install 只在主线程帧边界执行(建 GPU 管线);失败/编译失败都保留旧管线。
		for (const CompileOutcome& outcome : ready)
		{
			if (!outcome.Success)
			{
				WLD_CORE_WARN("[shader-hot-reload] compile failed key='{0}': {1}",
					outcome.LogicalPath, outcome.Error);
				continue;
			}
			const MaterialSurfaceRuntime::InstallResult install =
				MaterialSurfaceRuntime::Install(outcome.LogicalPath, outcome.Artifact);
			if (!install.Success)
			{
				WLD_CORE_WARN("[shader-hot-reload] install rejected key='{0}': {1}",
					outcome.LogicalPath, install.Error);
				continue;
			}
			const std::size_t version = MaterialSurfaceRuntime::PublishedVersion(outcome.LogicalPath);
			WLD_CORE_INFO("[shader-hot-reload] installed key='{0}' v{1} ({2:.1f} ms)",
				outcome.LogicalPath, version, outcome.ElapsedMilliseconds);
		}
	}

	void ShaderHotReload::Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_Shutdown)
				return;
			m_Shutdown = true;
		}
		m_Cv.notify_all();
		// 等在飞的编译结束(工作线程只跑 slangc / 读源,不碰 GPU;join 后不会再有产物)
		if (m_Worker.joinable())
			m_Worker.join();
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Pending.clear();
		m_PendingOrder.clear();
		m_Ready.clear();
		m_ReadyOrder.clear();
	}

	void ShaderHotReload::WorkerLoop()
	{
		for (;;)
		{
			CompileRequest request;
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
					continue;   // 不变量上到不了;防御性跳过,避免空转
				request = std::move(found->second);
				m_Pending.erase(found);
			}

			CompileOutcome outcome;
			outcome.Serial = request.Serial;
			outcome.LogicalPath = request.LogicalPath;
			try
			{
				// 工作线程只跑编译器(源/后端/include 根都来自主线程的请求副本);不碰 UI / 渲染 / VFS。
				const SurfaceCompileResult result = MaterialSurfaceCompiler::CompileSurface(
					request.Source, request.LogicalPath, request.Backend, request.IncludeRoots);
				outcome.Success = result.Success;
				outcome.Artifact = result.Artifact;
				outcome.ElapsedMilliseconds = result.ElapsedMilliseconds;
				if (!result.Success)
					outcome.Error = FirstDiagnosticText(result);
			}
			catch (const std::exception& exception)
			{
				// 编译器约定"失败不抛异常";这里只兜底线程边界(异常逃逸 = std::terminate)。
				outcome.Success = false;
				outcome.Error = std::string("compile threw: ") + exception.what();
			}
			catch (...)
			{
				outcome.Success = false;
				outcome.Error = "compile threw an unknown exception";
			}

			bool dropped = false;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_Shutdown)
					return;
				// 后来者胜:编译期间同路径又被入队 → 这份产物已经过期,直接丢(新请求随后编译)。
				if (m_Pending.find(request.LogicalPath) != m_Pending.end())
				{
					dropped = true;
				}
				else
				{
					const std::string& key = request.LogicalPath;
					const auto ready = m_Ready.find(key);
					if (ready == m_Ready.end())
					{
						m_Ready.emplace(key, std::move(outcome));
						m_ReadyOrder.push_back(key);
					}
					else
					{
						ready->second = std::move(outcome);   // 覆盖尚未被 Pump 消费的旧产物
					}
				}
			}
			if (dropped)
				WLD_CORE_INFO("[shader-hot-reload] dropped key='{0}' (a newer request superseded it)",
					request.LogicalPath);
		}
	}
}
