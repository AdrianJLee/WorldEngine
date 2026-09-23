#include "ShaderUtils.h"

#include "World/Renderer/Renderer.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace World
{
	namespace
	{
		namespace fs = std::filesystem;

		// 固定阶段表:烘焙产物只有 VSMain/PSMain 两个入口(其余入口
		// VSMainInstanced/VSMainSkinned 由 Renderer3D 在 CompileStage 里按名字请求)。
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
		// Slang-T2(2):引擎着色器从 dxc 单目标迁到 Slang 双目标 SPIR-V,旧缓存全部作废。
		constexpr uint32_t kCacheVersion = 2;

		// 着色器目标(Slang-T2 起每个 stage 两份 SPIR-V permutation):
		//  - Vulkan:Slang 的 Vulkan profile(SM6 语义;产物是 SPIR-V 1.3);
		//  - OpenGL:`<stage>_5_0+spirv_1_0` + `-fvk-use-gl-layout` —— ARB_gl_spirv
		//    只接受 SPIR-V 1.0,且不接受 OpTypeSampler(必须组合采样器;T1 实测:
		//    分离采样器形态会让 NVIDIA 驱动在首次采样时崩,14/14)。
		enum class ShaderTarget
		{
			Vulkan = 0,
			OpenGL = 1,
		};

		const char* TargetName(ShaderTarget target)
		{
			return target == ShaderTarget::OpenGL ? "opengl" : "vulkan";
		}

		// 从 "vs_6_0" / "ps_6_0" 取 stage 前缀("vs" / "ps")。
		std::string StagePrefix(const std::string& d3dProfile)
		{
			const size_t underscore = d3dProfile.find('_');
			return underscore == std::string::npos ? d3dProfile : d3dProfile.substr(0, underscore);
		}

		// 目标后端的 slangc profile:
		//  - Vulkan 直接用请求里的 D3D profile(vs_6_0/ps_6_0);
		//  - GL 强制 `<stage>_5_0+spirv_1_0`。
		std::string ProfileForTarget(ShaderTarget target, const std::string& d3dProfile)
		{
			if (target == ShaderTarget::Vulkan)
				return d3dProfile;
			return StagePrefix(d3dProfile) + "_5_0+spirv_1_0";
		}

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

		// 工具身份:用可执行文件大小做代理,工具升级(换 slangc/spirv-cross)时缓存自动失效。
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
			if (exe.empty())
			{
				exitCode = -1;
				return false;
			}
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

		// 每个逻辑路径只记一次"走了哪条路径",避免刷屏(但每条路径都可见)。
		void LogOnce(const std::string& key, const char* level, const std::string& message)
		{
			static std::mutex mutex;
			static std::set<std::string> reported;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!reported.insert(key).second)
					return;
			}
			if (!Log::GetCoreLogger())
				return;
			if (std::strcmp(level, "error") == 0)
				WLD_CORE_ERROR("{0}", message);
			else if (std::strcmp(level, "warn") == 0)
				WLD_CORE_WARN("{0}", message);
			else
				WLD_CORE_INFO("{0}", message);
		}

		// ---- Slang 工具解析(Slang-T2 过渡期;T5 的 FETCH + 根 CMake 变量接管) ----
		//
		// 顺序:
		//   1) WLD_SLANGC(完整 exe 路径)
		//   2) WLD_SLANG_DIR(目录)
		//   3) 编译期 WLD_SLANG_DIR(根 CMake 目前未定义;T3/T5 接入 FETCH 后自动生效)
		//   4) <repo>/vendor/tools/slang/slangc.exe(计划的 FETCH 落点)
		//   5) <repo 同级>/WorldEngine-deps/slang-*/bin/slangc.exe(本机开发依赖根)
		//   6) PATH 上的 slangc.exe
		// 解析结果只算一次并缓存;解析失败时把所有尝试过的路径打进 ERROR 日志。
		std::string ResolveSlangc()
		{
			std::vector<std::string> tried;
			const auto accept = [&tried](const fs::path& candidate) -> std::string
			{
				std::error_code ec;
				if (fs::is_regular_file(candidate, ec))
					return candidate.string();
				tried.push_back(candidate.string());
				return {};
			};

			if (const char* full = std::getenv("WLD_SLANGC"); full && *full)
				if (std::string resolved = accept(fs::path(full)); !resolved.empty())
					return resolved;
			if (const char* dir = std::getenv("WLD_SLANG_DIR"); dir && *dir)
				if (std::string resolved = accept(fs::path(dir) / "slangc.exe"); !resolved.empty())
					return resolved;
#ifdef WLD_SLANG_DIR
			if (std::string resolved = accept(fs::path(WLD_SLANG_DIR) / "slangc.exe"); !resolved.empty())
				return resolved;
#endif

			const fs::path repoRoot = WLD_REPO_ROOT;
			if (std::string resolved = accept(repoRoot / "vendor" / "tools" / "slang" / "slangc.exe");
				!resolved.empty())
				return resolved;

			std::error_code ec;
			const fs::path depsRoot = repoRoot.parent_path() / "WorldEngine-deps";
			std::vector<fs::path> versions;
			for (const fs::directory_entry& entry :
				fs::directory_iterator(depsRoot, fs::directory_options::skip_permission_denied, ec))
			{
				std::error_code entryEc;
				if (!entry.is_directory(entryEc))
					continue;
				const std::string name = entry.path().filename().string();
				if (name.rfind("slang-", 0) == 0)
					versions.push_back(entry.path());
			}
			std::sort(versions.begin(), versions.end());
			for (auto it = versions.rbegin(); it != versions.rend(); ++it)
				if (std::string resolved = accept(*it / "bin" / "slangc.exe"); !resolved.empty())
					return resolved;

#ifdef _WIN32
			{
				char buffer[MAX_PATH] = {};
				if (SearchPathA(nullptr, "slangc.exe", nullptr, MAX_PATH, buffer, nullptr) > 0)
					return std::string(buffer);
			}
#endif

			if (Log::GetCoreLogger())
			{
				std::string list;
				for (const std::string& path : tried)
					list += "\n  - " + path;
				WLD_CORE_ERROR("[shader] slangc.exe not found (Slang-T2 toolchain). Tried:{0}", list);
			}
			return {};
		}

		const std::string& SlangcPath()
		{
			static const std::string resolved = ResolveSlangc();
			return resolved;
		}

		// ---- SPIR-V 检查 ----
		bool SpirvLooksValid(const std::vector<uint8_t>& bytes)
		{
			if (bytes.size() < 20 || (bytes.size() % 4) != 0)
				return false;
			uint32_t magic = 0;
			std::memcpy(&magic, bytes.data(), sizeof(magic));
			return magic == 0x07230203u;
		}

		// ARB_gl_spirv 的"Non-acceptance of SPIR-V features"清单里有 OpTypeSampler:
		// Slang 的分离采样器形态(Texture2D + SamplerState)会产出它。T1 实测这种模块
		// 交给 glShaderBinary 之后,NVIDIA 驱动会在第一次采样时崩(nvoglv64+0x75abcb,
		// 14/14 同一签名)。所以这里做一次静态检查:命中就退回 GLSL 文本,而不是把
		// 一个已知会崩驱动的模块丢给驱动。
		bool SpirvHasSeparateSamplerType(const std::vector<uint8_t>& bytes)
		{
			if (!SpirvLooksValid(bytes))
				return false;
			// 头部 5 个字;之后每条指令 = (wordCount << 16) | opcode,OpTypeSampler = 26。
			const size_t words = bytes.size() / 4;
			for (size_t offset = 5; offset < words;)
			{
				uint32_t word = 0;
				std::memcpy(&word, bytes.data() + offset * 4, sizeof(word));
				const uint32_t wordCount = word >> 16;
				if (wordCount == 0)
					break;
				if ((word & 0xFFFFu) == 26u)
					return true;
				offset += wordCount;
			}
			return false;
		}

		// GL 目标 SPIR-V 的常规产物名(Slang-T2 定的规范名;与
		// shaders/<stem>.<Entry>.spv | .glsl 同一套命名)。
		// 当前 bake 仍只写 4 个产物/着色器(产物计数契约由
		// tests/World/ShaderPipelineTests.cpp 冻结);T5 的 FETCH/bake 会把它一并写进发行包,
		// 运行时这份解析已经就位(命中即用,不命中回落到内容寻址缓存)。
		std::string GlSpirVLogicalPath(const std::string& hlslPath, const std::string& entryPoint)
		{
			return std::string("shaders/") + StemOf(hlslPath) + "." + entryPoint + ".gl.spv";
		}

		// GL 侧的 SPIR-V 能力:由 RHI 设备能力位决定(带 GL_ARB_gl_spirv 的 GL 4.6 core)。
		bool GlSpirVModulesAvailable()
		{
			if (Renderer::GetAPI() != RendererAPI::API::OpenGL)
				return false;
			const Rhi::Handle<Rhi::Device> device = Renderer::GetDevice();
			return device && device->GetCapabilities().SpirVShaderModules;
		}
	}

	// WLD_DXC_DIR 由构建系统决定(dxc 只服务 MaterialSurface 路径;引擎着色器 T2 起走 Slang)。
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

		if (vulkan)
		{
			// 1) 烘焙产物(发行形态;宿主挂载 VFS 后注册)。
			const std::string logical = ArtifactLogicalPath(hlslPath, entryPoint, true);
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

			std::string modulePath;
			std::string error;
			if (!EnsureModule(sourceAbs, entryPoint, profile, /*glTarget*/ false, modulePath, error))
			{
				WLD_CORE_ERROR("Shader '{0}' ({1}) compile failed: {2}", hlslPath, entryPoint, error);
				WLD_CORE_ASSERT(false, "Shader compilation failed");
				return {};
			}
			return ReadBinaryFile(modulePath);
		}

		// ---- OpenGL ----
		// Slang-T2 起 GL 的默认摄入路径是 GL 目标 SPIR-V;只有拿不到它(能力缺失 /
		// 工具缺失 / 模块形态不合法)时才回退 GLSL 文本,并在 CompileStage 里留下 ERROR。
		std::vector<uint8_t> spirv;
		std::string reason;
		if (TryLoadGlSpirVStage(hlslPath, entryPoint, profile, spirv, reason))
			return std::vector<char>(spirv.begin(), spirv.end());

		if (s_Resolver)
		{
			const std::string logical = ArtifactLogicalPath(hlslPath, entryPoint, false);
			std::vector<uint8_t> bytes;
			if (s_Resolver(logical, bytes) && !bytes.empty())
			{
				s_CookedHits.fetch_add(1, std::memory_order_relaxed);
				LogCookedHit(logical, bytes.size());
				return std::vector<char>(bytes.begin(), bytes.end());
			}
		}

		std::error_code ec;
		const fs::path sourceAbs = fs::absolute(fs::path(WLD_WORLD_DIR) / hlslPath, ec);
		if (ec || !fs::is_regular_file(sourceAbs, ec))
		{
			WLD_CORE_ERROR("Shader '{0}' has no cooked artifact and no source in the content tree.", hlslPath);
			WLD_CORE_ASSERT(false, "Shader source missing and no cooked artifact available");
			return {};
		}

		std::string glslPath;
		std::string error;
		if (!EnsureGlslFallback(sourceAbs, entryPoint, profile, glslPath, error))
		{
			WLD_CORE_ERROR("Shader '{0}' ({1}) GLSL fallback failed: {2} (GL SPIR-V reason: {3})",
				hlslPath, entryPoint, error, reason);
			WLD_CORE_ASSERT(false, "Shader compilation failed");
			return {};
		}
		LogOnce("glsl-fallback:" + hlslPath + "." + entryPoint, "error",
			"[gl-spirv] " + hlslPath + " (" + entryPoint + "): using GLSL text fallback because " + reason);
		return ReadBinaryFile(glslPath);
	}

	Rhi::ShaderStageSource ShaderCompiler::CompileStage(Rhi::ShaderStage stage, const std::string& hlslPath,
		const std::string& entryPoint, const std::string& profile)
	{
		Rhi::ShaderStageSource out;
		out.Stage = stage;
		out.EntryPoint = entryPoint;

		if (Renderer::GetAPI() == RendererAPI::API::Vulkan)
		{
			// Vulkan:Slang 的 Vulkan profile 出 SPIR-V(模块入口名 = 源里的入口名,
			// 与 pipeline 的 pName 一致 —— slangc 用 -fvk-use-entrypoint-name)。
			const std::vector<char> bytes = CompileOrLoad(hlslPath, entryPoint, profile);
			out.SpirV.assign(bytes.begin(), bytes.end());
			return out;
		}

		// OpenGL:GL 目标 SPIR-V(默认路径)。
		std::vector<uint8_t> spirv;
		std::string reason;
		if (TryLoadGlSpirVStage(hlslPath, entryPoint, profile, spirv, reason))
		{
			out.SpirV = std::move(spirv);
			return out;
		}

		// 过渡期兜底(GLSL 文本;T6 删除):明确报出"走了哪条、为什么"。
		LogOnce("gl-spirv-fallback:" + hlslPath + "." + entryPoint, "error",
			"[gl-spirv] " + hlslPath + " (" + entryPoint + "): GL SPIR-V unavailable — " + reason +
			"; falling back to GLSL text (transitional, removed in T6)");

		if (s_Resolver)
		{
			const std::string logical = ArtifactLogicalPath(hlslPath, entryPoint, false);
			std::vector<uint8_t> bytes;
			if (s_Resolver(logical, bytes) && !bytes.empty())
			{
				s_CookedHits.fetch_add(1, std::memory_order_relaxed);
				LogCookedHit(logical, bytes.size());
				out.Glsl.assign(bytes.begin(), bytes.end());
				return out;
			}
		}

		std::error_code ec;
		const fs::path sourceAbs = fs::absolute(fs::path(WLD_WORLD_DIR) / hlslPath, ec);
		if (ec || !fs::is_regular_file(sourceAbs, ec))
		{
			WLD_CORE_ERROR("Shader '{0}' ({1}): no GL SPIR-V, no cooked GLSL and no source.", hlslPath, entryPoint);
			return out;
		}

		std::string glslPath;
		std::string error;
		if (!EnsureGlslFallback(sourceAbs, entryPoint, profile, glslPath, error))
		{
			WLD_CORE_ERROR("Shader '{0}' ({1}): GLSL fallback failed: {2}", hlslPath, entryPoint, error);
			return out;
		}
		const std::vector<char> glsl = ReadBinaryFile(glslPath);
		out.Glsl.assign(glsl.begin(), glsl.end());
		return out;
	}

	bool ShaderCompiler::TryLoadGlSpirVStage(const std::string& hlslPath, const std::string& entryPoint,
		const std::string& profile, std::vector<uint8_t>& out, std::string& reason)
	{
		if (!GlSpirVModulesAvailable())
		{
			reason = "GL_SPIRV capability missing (need GL 4.6 core + GL_ARB_gl_spirv and a live device)";
			return false;
		}

		// 1) 烘焙产物:shaders/<stem>.<Entry>.gl.spv(与 .spv/.glsl 同一套命名规则)。
		if (s_Resolver)
		{
			const std::string logical = GlSpirVLogicalPath(hlslPath, entryPoint);
			std::vector<uint8_t> bytes;
			if (s_Resolver(logical, bytes) && !bytes.empty())
			{
				if (!SpirvLooksValid(bytes))
				{
					reason = "cooked GL module is not a SPIR-V module: " + logical;
					return false;
				}
				if (SpirvHasSeparateSamplerType(bytes))
				{
					reason = "cooked GL module declares OpTypeSampler (ARB_gl_spirv rejects it): " + logical;
					return false;
				}
				s_CookedHits.fetch_add(1, std::memory_order_relaxed);
				LogCookedHit(logical, bytes.size());
				out = std::move(bytes);
				return true;
			}
		}

		// 2) 内容寻址缓存(开发形态:slangc 现场编译 GL 目标)。
		std::error_code ec;
		const fs::path sourceAbs = fs::absolute(fs::path(WLD_WORLD_DIR) / hlslPath, ec);
		if (ec || !fs::is_regular_file(sourceAbs, ec))
		{
			reason = "shader source missing in the content tree: " + hlslPath;
			return false;
		}

		std::string modulePath;
		std::string error;
		if (!EnsureModule(sourceAbs, entryPoint, profile, /*glTarget*/ true, modulePath, error))
		{
			reason = error;
			return false;
		}

		const std::vector<char> bytes = ReadAllBytes(modulePath);
		std::vector<uint8_t> module(bytes.begin(), bytes.end());
		if (!SpirvLooksValid(module))
		{
			reason = "compiled GL module is not a SPIR-V module: " + modulePath;
			return false;
		}
		if (SpirvHasSeparateSamplerType(module))
		{
			// 源码还不是 Slang 形态(例如手工放进来的老 HLSL)。GL 上这种模块会让驱动崩,
			// 必须退回 GLSL 文本 —— 日志里给出可操作的提示。
			reason = "source produced OpTypeSampler (use Slang combined samplers `Sampler2D`): " + hlslPath;
			return false;
		}

		out = std::move(module);
		return true;
	}

	bool ShaderCompiler::EnsureModule(const std::filesystem::path& sourceAbs, const std::string& entryPoint,
		const std::string& profile, bool glTarget, std::string& outPath, std::string& error)
	{
		const ShaderTarget target = glTarget ? ShaderTarget::OpenGL : ShaderTarget::Vulkan;
		const std::vector<char> source = ReadAllBytes(sourceAbs.string());
		if (source.empty())
		{
			error = "cannot read source: " + sourceAbs.string();
			return false;
		}

		const std::string targetProfile = ProfileForTarget(target, profile);
		uint64_t fingerprint = Fnv1a64(source.data(), source.size());
		fingerprint = Mix(fingerprint, entryPoint);
		fingerprint = Mix(fingerprint, targetProfile);
		fingerprint = Mix(fingerprint, TargetName(target));
		fingerprint = Mix(fingerprint, std::to_string(kCacheVersion));
		fingerprint = Mix(fingerprint, std::to_string(ToolIdentity(SlangcPath())));

		const fs::path cacheDir(cacheDirAbsPath);
		std::error_code ec;
		fs::create_directories(cacheDir, ec);
		if (ec)
		{
			error = "cannot create cache dir: " + ec.message();
			return false;
		}

		const std::string base = Hex(fingerprint);
		outPath = (cacheDir / (base + (glTarget ? ".gl.spv" : ".vk.spv"))).string();
		if (fs::is_regular_file(outPath, ec))
		{
			s_CacheHits.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		if (SlangcPath().empty())
		{
			error = "slangc.exe was not found (Slang-T2 toolchain; set WLD_SLANG_DIR)";
			return false;
		}

		std::string arguments = "-target spirv -profile \"" + targetProfile + "\" -entry \"" + entryPoint +
			"\" \"" + sourceAbs.string() + "\" -o \"" + outPath + "\"";
		if (glTarget)
		{
			// GL 目标:保留 T1 验证过的 `-fvk-use-gl-layout`;入口名保持 Slang 默认的
			// "main"(ARB_gl_spirv 的 glSpecializeShader 固定用这个名字)。
			arguments = "-target spirv -profile \"" + targetProfile + "\" -fvk-use-gl-layout -entry \"" +
				entryPoint + "\" \"" + sourceAbs.string() + "\" -o \"" + outPath + "\"";
		}
		else
		{
			// Vulkan 目标:模块入口名 = 源里的入口名(pipeline 的 pName 直接用 EntryPoint)。
			arguments = "-target spirv -profile \"" + targetProfile + "\" -fvk-use-entrypoint-name -entry \"" +
				entryPoint + "\" \"" + sourceAbs.string() + "\" -o \"" + outPath + "\"";
		}

		s_ToolInvocations.fetch_add(1, std::memory_order_relaxed);
		int result = 0;
		if (!RunTool(SlangcPath(), arguments, result))
		{
			error = "cannot launch slangc: " + SlangcPath();
			return false;
		}
		if (result != 0)
		{
			std::error_code removeEc;
			fs::remove(outPath, removeEc);
			error = "slangc failed (" + std::to_string(result) + ") for " + sourceAbs.string() + " (" +
				entryPoint + ", " + TargetName(target) + ")";
			return false;
		}
		if (!fs::is_regular_file(outPath, ec))
		{
			error = "slangc produced no module: " + outPath;
			return false;
		}

		LogOnce("slang-module:" + outPath, "info",
			std::string("[shader] ") + TargetName(target) + " module compiled: " + outPath);
		return true;
	}

	bool ShaderCompiler::EnsureGlslFallback(const std::filesystem::path& sourceAbs, const std::string& entryPoint,
		const std::string& profile, std::string& outPath, std::string& error)
	{
		std::string spvPath;
		if (!EnsureModule(sourceAbs, entryPoint, profile, /*glTarget*/ false, spvPath, error))
			return false;

		uint64_t fingerprint = Fnv1a64String(spvPath);
		fingerprint = Mix(fingerprint, std::to_string(ToolIdentity(spirvCrossAbsPath)));

		const fs::path cacheDir(cacheDirAbsPath);
		std::error_code ec;
		outPath = (cacheDir / (Hex(fingerprint) + ".glsl")).string();
		if (fs::is_regular_file(outPath, ec))
		{
			s_CacheHits.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		s_ToolInvocations.fetch_add(1, std::memory_order_relaxed);
		if (!CrossCompileToGlsl(spvPath, outPath))
		{
			error = "spirv-cross failed for " + spvPath;
			return false;
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
				std::string cachedSpv;
				std::string error;
				if (!EnsureModule(entry.path(), stage.Entry, stage.Profile, /*glTarget*/ false, cachedSpv, error))
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = error;
					continue;
				}
				std::string cachedGlsl;
				if (!EnsureGlslFallback(entry.path(), stage.Entry, stage.Profile, cachedGlsl, error))
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = error;
					continue;
				}

				const std::string stem = StemOf(entry.path().string());
				const fs::path spvOut = outShaders / (stem + "." + stage.Suffix + ".spv");
				const fs::path glslOut = outShaders / (stem + "." + stage.Suffix + ".glsl");
				fs::copy_file(cachedSpv, spvOut, fs::copy_options::overwrite_existing, ec);
				if (ec)
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = "cannot write " + spvOut.string() + ": " + ec.message();
					continue;
				}
				fs::copy_file(cachedGlsl, glslOut, fs::copy_options::overwrite_existing, ec);
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

	bool ShaderCompiler::CompileToSpvWithDxc(const std::string& hlslAbsPath, const std::string& entryPoint,
		const std::string& profile, const std::string& spvAbsPath)
	{
		// Slang-T2 起引擎着色器不再经过 dxc(源码已是 Slang 形态:Sampler2D + [[vk::binding]])。
		// 这个函数与 dxcAbsPath 保留到 T6,与工具链一并删除。
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
