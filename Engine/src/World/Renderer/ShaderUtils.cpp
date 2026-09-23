#include "ShaderUtils.h"

#include "World/Renderer/Renderer.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif

namespace World
{
	namespace
	{
		namespace fs = std::filesystem;

		// 固定阶段表:当前所有引擎着色器都用 VSMain/PSMain 两个入口。
		struct StageEntry
		{
			const char* Entry;
			const char* Profile;
			const char* Suffix;   // 产物文件后缀(与入口名一致)
		};
		constexpr StageEntry kStages[] = {
			{ "VSMain", "vs_6_0", "VSMain" },
			{ "PSMain", "ps_6_0", "PSMain" },
		};

		// 缓存版本:编译命令/工具链语义变化时递增,旧缓存自动失效。
		constexpr uint32_t kCacheVersion = 1;

		std::atomic<size_t> s_CookedHits { 0 };
		std::atomic<size_t> s_CacheHits { 0 };
		std::atomic<size_t> s_ToolInvocations { 0 };

		ShaderCompiler::ArtifactResolver s_Resolver;

		uint64_t Fnv1a64(const void* data, size_t size)
		{
			uint64_t hash = 14695981039346656037ULL;
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		uint64_t Fnv1a64String(const std::string& text)
		{
			return Fnv1a64(text.data(), text.size());
		}

		std::string Hex(uint64_t value)
		{
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
			return buffer;
		}

		uint64_t Mix(uint64_t hash, const std::string& text)
		{
			hash ^= Fnv1a64String(text);
			hash *= 1099511628211ULL;
			return hash;
		}

		// 工具身份:用可执行文件大小做代理,工具升级(换 dxc/spirv-cross)时缓存自动失效。
		uint64_t ToolIdentity(const std::string& toolPath)
		{
			std::error_code ec;
			const uint64_t size = static_cast<uint64_t>(fs::file_size(toolPath, ec));
			return ec ? 0 : size;
		}

		std::vector<char> ReadAllBytes(const std::string& filename)
		{
			std::ifstream file(filename, std::ios::ate | std::ios::binary);
			if (!file.is_open())
				return {};
			const size_t fileSize = static_cast<size_t>(file.tellg());
			if (fileSize == 0)
				return {};
			std::vector<char> buffer(fileSize);
			file.seekg(0);
			file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
			return buffer;
		}

		std::string StemOf(const std::string& hlslPath)
		{
			return fs::path(hlslPath).stem().string();
		}

		// 直接 CreateProcess 调用工具:std::system 经 cmd.exe 时,
		// "带引号的程序路径 + 参数" 会被 cmd 的引号剥离规则破坏。
		bool RunTool(const std::string& exe, const std::string& arguments, int& exitCode)
		{
#ifdef _WIN32
			std::string commandLine = "\"" + exe + "\" " + arguments;
			STARTUPINFOA startup {};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process {};
			if (!CreateProcessA(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
				0, nullptr, nullptr, &startup, &process))
			{
				exitCode = -1;
				return false;
			}
			WaitForSingleObject(process.hProcess, INFINITE);
			DWORD code = 0;
			GetExitCodeProcess(process.hProcess, &code);
			CloseHandle(process.hProcess);
			CloseHandle(process.hThread);
			exitCode = static_cast<int>(code);
			return true;
#else
			const std::string commandLine = "\"" + exe + "\" " + arguments;
			exitCode = std::system(commandLine.c_str());
			return true;
#endif
		}

		// 每个逻辑路径只记一次"命中烘焙产物",便于验收日志核对。
		void LogCookedHit(const std::string& logical, size_t size)
		{
			static std::mutex mutex;
			static std::set<std::string> reported;
			std::lock_guard<std::mutex> lock(mutex);
			if (!reported.insert(logical).second)
				return;
			// 无头测试等场景可能未初始化日志,这里跟随现有守卫模式。
			if (Log::GetCoreLogger())
				WLD_CORE_INFO("[Shader] cooked artifact hit: {0} ({1} bytes)", logical, size);
		}

		// ---- T1 试点:GL 直接摄入 SPIR-V(GL 4.6 core + GL_ARB_gl_spirv) ----
		//
		// 试点期由离线脚本(见 tools/agents/scratch/slang-t1/build-gl-spirv.ps1)用 slangc 产出
		// `<stem>.<EntryPoint>.spv` 到 WLD_GL_SPIRV_DIR;每个后端一份 SPIR-V permutation
		// (GL 侧:DescriptorSet=0、binding=GL 单元号、无 -fvk-invert-y)。T3 起这段由
		// slangc 现场编译 + 缓存键(目标后端/工具身份/契约版本)替代,本函数即那时的解析器。
		bool GlSpirVModulesAvailable()
		{
			if (Renderer::GetAPI() != RendererAPI::API::OpenGL)
				return false;
			const Rhi::Handle<Rhi::Device> device = Renderer::GetDevice();
			return device && device->GetCapabilities().SpirVShaderModules;
		}

		bool TryLoadGlSpirVStage(const std::string& hlslPath, const std::string& entryPoint,
			std::vector<uint8_t>& out)
		{
			if (!GlSpirVModulesAvailable())
				return false;
			const char* dir = std::getenv("WLD_GL_SPIRV_DIR");
			if (!dir || !*dir)
				return false;

			const fs::path modulePath = fs::path(dir) / (StemOf(hlslPath) + "." + entryPoint + ".spv");
			const std::vector<char> bytes = ReadAllBytes(modulePath.string());
			if (bytes.empty() || (bytes.size() % 4) != 0)
			{
				static std::mutex mutex;
				static std::set<std::string> reported;
				std::lock_guard<std::mutex> lock(mutex);
				if (reported.insert(modulePath.string()).second && Log::GetCoreLogger())
					WLD_CORE_WARN("[gl-spirv] pilot module unavailable: {0} (falling back to GLSL text)",
						modulePath.string());
				return false;
			}

			out.assign(bytes.begin(), bytes.end());
			static std::mutex mutex;
			static std::set<std::string> reported;
			std::lock_guard<std::mutex> lock(mutex);
			if (reported.insert(modulePath.string()).second && Log::GetCoreLogger())
				WLD_CORE_INFO("[gl-spirv] stage source: {0} ({1} bytes) -> SPIR-V module path",
					modulePath.string(), out.size());
			return true;
		}
	}

	// WLD_DXC_DIR 由构建系统决定(优先 Vulkan SDK 完整安装,含匹配的 dxcompiler.dll)。
	const std::string ShaderCompiler::dxcAbsPath = std::string(WLD_DXC_DIR) + "dxc.exe";
// 工具目录由构建系统给(WLD_SPIRV_CROSS_DIR):spirv-cross 属**可替换**工具,换实现只改 CMake 变量。
const std::string ShaderCompiler::spirvCrossAbsPath = std::string(WLD_SPIRV_CROSS_DIR) + "spirv-cross.exe";
	const std::string ShaderCompiler::cacheDirAbsPath = WLD_INTERMEDIATE_DIR + std::string("ShaderCache/");

	std::string ShaderCompiler::ArtifactLogicalPath(const std::string& hlslPath, const std::string& entryPoint, bool vulkan)
	{
		return std::string("shaders/") + StemOf(hlslPath) + "." + entryPoint + (vulkan ? ".spv" : ".glsl");
	}

	void ShaderCompiler::SetArtifactResolver(ArtifactResolver resolver)
	{
		s_Resolver = std::move(resolver);
	}

	void ShaderCompiler::ClearArtifactResolver()
	{
		s_Resolver = nullptr;
	}

	size_t ShaderCompiler::CookedHitCount() { return s_CookedHits.load(std::memory_order_relaxed); }
	size_t ShaderCompiler::CacheHitCount() { return s_CacheHits.load(std::memory_order_relaxed); }
	size_t ShaderCompiler::ToolInvocationCount() { return s_ToolInvocations.load(std::memory_order_relaxed); }

	void ShaderCompiler::ResetCounters()
	{
		s_CookedHits.store(0, std::memory_order_relaxed);
		s_CacheHits.store(0, std::memory_order_relaxed);
		s_ToolInvocations.store(0, std::memory_order_relaxed);
	}

	std::vector<char> ShaderCompiler::CompileOrLoad(const std::string& hlslPath, const std::string& entryPoint,
		const std::string& profile)
	{
		const bool vulkan = Renderer::GetAPI() == RendererAPI::API::Vulkan;
		const std::string logical = ArtifactLogicalPath(hlslPath, entryPoint, vulkan);

		// 1) 烘焙产物(发行形态;宿主挂载 VFS 后注册)。
		if (s_Resolver)
		{
			std::vector<uint8_t> bytes;
			if (s_Resolver(logical, bytes) && !bytes.empty())
			{
				s_CookedHits.fetch_add(1, std::memory_order_relaxed);
				LogCookedHit(logical, bytes.size());
				return std::vector<char>(bytes.begin(), bytes.end());
			}
		}

		// 2) 源码树按需编译(开发形态)。
		std::error_code ec;
		const fs::path sourceAbs = fs::absolute(fs::path(WLD_WORLD_DIR) / hlslPath, ec);
		if (ec || !fs::is_regular_file(sourceAbs, ec))
		{
			WLD_CORE_ERROR("Shader '{0}' has no cooked artifact ('{1}') and no source in the content tree.",
				hlslPath, logical);
			WLD_CORE_ASSERT(false, "Shader source missing and no cooked artifact available");
			return {};
		}

		CachedArtifacts cached;
		std::string error;
		if (!EnsureCached(sourceAbs, entryPoint, profile, cached, error))
		{
			WLD_CORE_ERROR("Shader '{0}' ({1}) compile failed: {2}", hlslPath, entryPoint, error);
			WLD_CORE_ASSERT(false, "Shader compilation failed");
			return {};
		}
		return ReadBinaryFile(vulkan ? cached.Spv : cached.Glsl);
	}

	Rhi::ShaderStageSource ShaderCompiler::CompileStage(Rhi::ShaderStage stage, const std::string& hlslPath,
		const std::string& entryPoint, const std::string& profile)
	{
		Rhi::ShaderStageSource out;
		out.Stage = stage;
		out.EntryPoint = entryPoint;

		// T1 试点:命中 GL 的 SPIR-V 模块时不再调用 dxc/spirv-cross(GLSL 字段留空)。
		if (TryLoadGlSpirVStage(hlslPath, entryPoint, out.SpirV))
			return out;

		const std::vector<char> bytes = CompileOrLoad(hlslPath, entryPoint, profile);
		if (Renderer::GetAPI() == RendererAPI::API::Vulkan)
			out.SpirV.assign(bytes.begin(), bytes.end());
		else
			out.Glsl.assign(bytes.begin(), bytes.end());
		return out;
	}

	bool ShaderCompiler::EnsureCached(const std::filesystem::path& sourceAbs, const std::string& entryPoint,
		const std::string& profile, CachedArtifacts& out, std::string& error)
	{
		const std::vector<char> source = ReadAllBytes(sourceAbs.string());
		if (source.empty())
		{
			error = "cannot read source: " + sourceAbs.string();
			return false;
		}

		uint64_t fingerprint = Fnv1a64(source.data(), source.size());
		fingerprint = Mix(fingerprint, entryPoint);
		fingerprint = Mix(fingerprint, profile);
		fingerprint = Mix(fingerprint, std::to_string(kCacheVersion));
		fingerprint = Mix(fingerprint, std::to_string(ToolIdentity(dxcAbsPath)));
		fingerprint = Mix(fingerprint, std::to_string(ToolIdentity(spirvCrossAbsPath)));

		const fs::path cacheDir(cacheDirAbsPath);
		std::error_code ec;
		fs::create_directories(cacheDir, ec);
		if (ec)
		{
			error = "cannot create cache dir: " + ec.message();
			return false;
		}

		const std::string base = Hex(fingerprint);
		out.Spv = (cacheDir / (base + ".spv")).string();
		out.Glsl = (cacheDir / (base + ".glsl")).string();

		if (fs::is_regular_file(out.Spv, ec) && fs::is_regular_file(out.Glsl, ec))
		{
			s_CacheHits.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		if (!fs::is_regular_file(out.Spv, ec))
		{
			s_ToolInvocations.fetch_add(1, std::memory_order_relaxed);
			if (!CompileToSpv(sourceAbs.string(), entryPoint, profile, out.Spv))
			{
				error = "dxc failed for " + sourceAbs.string() + " (" + entryPoint + ")";
				return false;
			}
		}
		if (!fs::is_regular_file(out.Glsl, ec))
		{
			s_ToolInvocations.fetch_add(1, std::memory_order_relaxed);
			if (!CrossCompileToGlsl(out.Spv, out.Glsl))
			{
				error = "spirv-cross failed for " + out.Spv;
				return false;
			}
		}
		return true;
	}

	ShaderCompiler::BakeResult ShaderCompiler::BakeDirectory(const std::filesystem::path& sourceDir,
		const std::filesystem::path& outputDir)
	{
		BakeResult result;
		std::error_code ec;
		if (!fs::is_directory(sourceDir, ec))
		{
			result.Error = "shader source directory missing: " + sourceDir.string();
			result.Failed = 1;
			return result;
		}

		const fs::path outShaders = outputDir / "shaders";
		fs::create_directories(outShaders, ec);
		if (ec)
		{
			result.Error = "cannot create shader output dir: " + ec.message();
			result.Failed = 1;
			return result;
		}

		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(
			sourceDir, fs::directory_options::skip_permission_denied, ec))
		{
			if (!entry.is_regular_file(ec) || entry.path().extension() != ".hlsl")
				continue;

			++result.Shaders;
			for (const StageEntry& stage : kStages)
			{
				CachedArtifacts cached;
				std::string error;
				if (!EnsureCached(entry.path(), stage.Entry, stage.Profile, cached, error))
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = error;
					continue;
				}

				const std::string stem = StemOf(entry.path().string());
				const fs::path spvOut = outShaders / (stem + "." + stage.Suffix + ".spv");
				const fs::path glslOut = outShaders / (stem + "." + stage.Suffix + ".glsl");
				fs::copy_file(cached.Spv, spvOut, fs::copy_options::overwrite_existing, ec);
				if (ec)
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = "cannot write " + spvOut.string() + ": " + ec.message();
					continue;
				}
				fs::copy_file(cached.Glsl, glslOut, fs::copy_options::overwrite_existing, ec);
				if (ec)
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = "cannot write " + glslOut.string() + ": " + ec.message();
					continue;
				}
				result.Artifacts += 2;
			}
		}
		return result;
	}

	bool ShaderCompiler::CompileToSpv(const std::string& hlslAbsPath, const std::string& entryPoint,
		const std::string& profile, const std::string& spvAbsPath)
	{
		const std::string arguments = "-spirv -T " + profile + " -E " + entryPoint +
			" \"" + hlslAbsPath + "\" -Fo \"" + spvAbsPath + "\"";
		int result = 0;
		if (!RunTool(dxcAbsPath, arguments, result))
		{
			std::fprintf(stderr, "[ShaderCompiler] cannot launch dxc: %s\n", dxcAbsPath.c_str());
			return false;
		}
		if (result != 0)
		{
			std::fprintf(stderr, "[ShaderCompiler] dxc failed (%d): %s %s\n", result,
				dxcAbsPath.c_str(), arguments.c_str());
			return false;
		}
		return true;
	}

	bool ShaderCompiler::CrossCompileToGlsl(const std::string& spvAbsPath, const std::string& glslAbsPath)
	{
		const std::string arguments = "--version 450 --combined-samplers-inherit-bindings --output \"" +
			glslAbsPath + "\" \"" + spvAbsPath + "\"";
		int result = 0;
		if (!RunTool(spirvCrossAbsPath, arguments, result))
		{
			std::fprintf(stderr, "[ShaderCompiler] cannot launch spirv-cross: %s\n", spirvCrossAbsPath.c_str());
			return false;
		}
		if (result != 0)
		{
			std::fprintf(stderr, "[ShaderCompiler] spirv-cross failed (%d): %s %s\n", result,
				spirvCrossAbsPath.c_str(), arguments.c_str());
			return false;
		}
		return true;
	}

	bool ShaderCompiler::WriteBinaryFile(const std::string& filename, const std::vector<char>& data)
	{
		std::ofstream stream(filename, std::ios::binary | std::ios::trunc);
		if (!stream)
			return false;
		stream.write(data.data(), static_cast<std::streamsize>(data.size()));
		return static_cast<bool>(stream);
	}

	std::vector<char> ShaderCompiler::ReadBinaryFile(const std::string& filename)
	{
		std::vector<char> buffer = ReadAllBytes(filename);
		if (buffer.empty())
			WLD_CORE_ERROR("Failed to read shader binary: {0}", filename);
		return buffer;
	}
}
