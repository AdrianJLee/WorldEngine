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
		// Slang-T2(2):引擎着色器从单目标迁到 Slang 双目标 SPIR-V,旧缓存全部作废。
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

		// 工具身份:用可执行文件大小做代理,工具升级(换 slangc 构建)时缓存自动失效。
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

		// ---- Slang 工具解析(Slang-T5:唯一入口) ----
		//
		// 只有一处事实源:构建系统给的 WLD_SLANG_DIR(根 CMake 变量,可 -D 覆盖;
		// 默认 <repo>/../WorldEngine-deps/slang-<版本>/bin,由 tools/agents/fetch-slang.ps1
		// 按版本 + sha256 取到仓库外)。T2/T3 过渡期的 env(WLD_SLANGC/WLD_SLANG_DIR)、
		// vendor/tools/slang、同级 WorldEngine-deps 版本扫描与 PATH 兜底全部删除 ——
		// "去哪儿找工具"由配置决定一次,不在运行时猜。
		// 解析结果只算一次并缓存;失败时给出可执行的修复提示。
		std::string ResolveSlangc()
		{
#ifdef WLD_SLANG_DIR
			const fs::path toolDir(WLD_SLANG_DIR);
			const fs::path candidate = toolDir / "slangc.exe";
			std::error_code ec;
			if (fs::is_regular_file(candidate, ec))
				return candidate.string();
			if (Log::GetCoreLogger())
			{
				WLD_CORE_ERROR("[shader] slangc.exe not found at '{0}' (WLD_SLANG_DIR). "
					"Run tools/agents/fetch-slang.ps1, or configure with -DWLD_SLANG_DIR=<dir>.",
					candidate.string());
			}
#else
			if (Log::GetCoreLogger())
			{
				WLD_CORE_ERROR("[shader] built without WLD_SLANG_DIR: no shader compiler directory is "
					"configured. Reconfigure with -DWLD_SLANG_DIR=<dir containing slangc.exe>.");
			}
#endif
			return {};
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
		// 14/14 同一签名)。所以这里做一次静态检查:命中就**拒绝**这个模块,而不是把一个
		// 已知会崩驱动的模块丢给驱动(T6b 起没有 GLSL 文本兜底:拒绝 = 该阶段装配失败)。
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

		// Slang-T5:写出 GL 目标产物**之前**的形态判据(与运行时 TryLoadGlSpirVStage 同一套):
		// ARB_gl_spirv 只吃 SPIR-V 1.0,且不接受 OpTypeSampler。不合法就不写进发行包 ——
		// 让打包阶段失败,而不是让用户机器上的驱动去崩。
		bool SpirvIsGlIngestable(const std::vector<char>& bytes, std::string& reason)
		{
			if (bytes.size() < 20 || (bytes.size() % 4) != 0)
			{
				reason = "not a SPIR-V module (bad size)";
				return false;
			}
			uint32_t magic = 0;
			std::memcpy(&magic, bytes.data(), sizeof(magic));
			if (magic != 0x07230203u)
			{
				reason = "not a SPIR-V module (bad magic)";
				return false;
			}
			uint32_t version = 0;
			std::memcpy(&version, bytes.data() + 4, sizeof(version));
			if (version != 0x00010000u)
			{
				char text[96] = {};
				std::snprintf(text, sizeof(text), "SPIR-V version word 0x%08x is not 1.0 "
					"(ARB_gl_spirv needs 1.0)", version);
				reason = text;
				return false;
			}
			const std::vector<uint8_t> asBytes(bytes.begin(), bytes.end());
			if (SpirvHasSeparateSamplerType(asBytes))
			{
				reason = "module declares OpTypeSampler (GL needs the combined `Sampler2D` form)";
				return false;
			}
			return true;
		}

		// 运行时会请求的入口点:VSMain / PSMain 固定;实例化/蒙皮入口只在源码里真的声明了
		// 才烘(只有 Renderer3D_Solid / Renderer3D_Shadow 有 —— 无脑烘会让 slangc 报
		// "entry point not found",把健康的着色器判成失败)。
		std::vector<std::string> DiscoverEntryPoints(const std::string& sourceText)
		{
			std::vector<std::string> entries { "VSMain", "PSMain" };
			if (sourceText.find("VSMainInstanced") != std::string::npos)
				entries.push_back("VSMainInstanced");
			if (sourceText.find("VSMainSkinned") != std::string::npos)
				entries.push_back("VSMainSkinned");
			return entries;
		}

		const char* ProfileForEntry(const std::string& entryPoint)
		{
			return entryPoint.rfind("VS", 0) == 0 ? "vs_6_0" : "ps_6_0";
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

	const std::string ShaderCompiler::cacheDirAbsPath = WLD_INTERMEDIATE_DIR + std::string("ShaderCache/");

	std::string ShaderCompiler::ArtifactLogicalPath(const std::string& hlslPath, const std::string& entryPoint, bool vulkan)
	{
		// Slang-T6b:两个目标都是 SPIR-V 产物 —— 命名只有这一处实现(cook 写、运行时读)。
		return std::string("shaders/") + StemOf(hlslPath) + "." + entryPoint + (vulkan ? ".spv" : ".gl.spv");
	}

	const std::string& ShaderCompiler::SlangcPath()
	{
		// 唯一入口:解析一次并缓存(路径来自构建期的 WLD_SLANG_DIR,见 ResolveSlangc 的说明)。
		static const std::string resolved = ResolveSlangc();
		return resolved;
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

		// ---- OpenGL:GL 目标 SPIR-V(GL 4.6 core + GL_ARB_gl_spirv)----
		// 烘焙产物 → 内容寻址缓存 → slangc 现场编译;两条都拿不到就是失败。
		// Slang-T6b 起没有 GLSL 文本兜底(旧的编译器与转译器已删除)。
		std::vector<uint8_t> spirv;
		std::string reason;
		if (TryLoadGlSpirVStage(hlslPath, entryPoint, profile, spirv, reason))
			return std::vector<char>(spirv.begin(), spirv.end());

		WLD_CORE_ERROR("Shader '{0}' ({1}): no GL SPIR-V module available: {2}", hlslPath, entryPoint, reason);
		WLD_CORE_ASSERT(false, "GL shader stage has no SPIR-V module");
		return {};
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

		// OpenGL:GL 目标 SPIR-V(唯一路径)。
		std::vector<uint8_t> spirv;
		std::string reason;
		if (TryLoadGlSpirVStage(hlslPath, entryPoint, profile, spirv, reason))
		{
			out.SpirV = std::move(spirv);
			return out;
		}

		// Slang-T6b:没有 GLSL 文本兜底 —— 留 ERROR,返回空阶段(调用方按"该阶段不可用"处理)。
		WLD_CORE_ERROR("Shader '{0}' ({1}): GL SPIR-V unavailable — {2}", hlslPath, entryPoint, reason);
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

		// 1) 烘焙产物:shaders/<stem>.<Entry>.gl.spv(命名见 ArtifactLogicalPath)。
		if (s_Resolver)
		{
			const std::string logical = ArtifactLogicalPath(hlslPath, entryPoint, /*vulkan*/ false);
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
			// 必须拒绝 —— 日志里给出可操作的提示(T6b 起没有 GLSL 文本兜底)。
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
		fingerprint = Mix(fingerprint, std::to_string(ToolIdentity(ShaderCompiler::SlangcPath())));

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

		if (ShaderCompiler::SlangcPath().empty())
		{
			error = "slangc.exe was not found in WLD_SLANG_DIR (run tools/agents/fetch-slang.ps1)";
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
		if (!RunTool(ShaderCompiler::SlangcPath(), arguments, result))
		{
			error = "cannot launch slangc: " + ShaderCompiler::SlangcPath();
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

	ShaderCompiler::BakeResult ShaderCompiler::BakeDirectory(const std::filesystem::path& sourceDir,
		const std::filesystem::path& outputDir)
	{
		// Slang-T6b:兼容入口(EditorCooker 的 4a 步)。只烘 Vulkan 目标 shaders/<stem>.<entry>.spv;
		// GLSL 文本产物已删除,GL 目标 .gl.spv 由 BakeDistributionTargets 写。
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

				const std::string stem = StemOf(entry.path().string());
				const fs::path spvOut = outShaders / (stem + "." + stage.Suffix + ".spv");
				fs::copy_file(cachedSpv, spvOut, fs::copy_options::overwrite_existing, ec);
				if (ec)
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = "cannot write " + spvOut.string() + ": " + ec.message();
					continue;
				}
				++result.Artifacts;
			}
		}
		return result;
	}

	ShaderCompiler::BakeResult ShaderCompiler::BakeDistributionTargets(const std::filesystem::path& sourceDir,
		const std::filesystem::path& outputDir)
	{
		// Slang-T5:发行形态的"每个入口两份 SPIR-V"。Vulkan 目标给 Vulkan RHI;
		// GL 目标(SPIR-V 1.0 + 组合 Sampler2D + 入口名 "main")给 GL 4.6 的
		// glShaderBinary/glSpecializeShader。两者与开发形态用**同一个**内容寻址缓存,
		// 所以重复 cook 不重编译,产物与现场编译逐字节相同。
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

			const std::vector<char> sourceBytes = ReadAllBytes(entry.path().string());
			if (sourceBytes.empty())
			{
				++result.Failed;
				if (result.Error.empty())
					result.Error = "cannot read shader source: " + entry.path().string();
				continue;
			}
			const std::string sourceText(sourceBytes.begin(), sourceBytes.end());
			const std::string stem = StemOf(entry.path().string());
			++result.Shaders;

			for (const std::string& entryPoint : DiscoverEntryPoints(sourceText))
			{
				for (const bool glTarget : { false, true })
				{
					std::string cachedPath;
					std::string error;
					if (!EnsureModule(entry.path(), entryPoint, ProfileForEntry(entryPoint), glTarget,
							cachedPath, error))
					{
						++result.Failed;
						if (result.Error.empty())
							result.Error = error;
						continue;
					}

					const std::vector<char> module = ReadAllBytes(cachedPath);
					if (module.empty())
					{
						++result.Failed;
						if (result.Error.empty())
							result.Error = "empty module: " + cachedPath;
						continue;
					}
					if (glTarget)
					{
						std::string reason;
						if (!SpirvIsGlIngestable(module, reason))
						{
							++result.Failed;
							if (result.Error.empty())
								result.Error = stem + "." + entryPoint + " GL module rejected: " + reason;
							continue;
						}
					}

					const fs::path artifact = outShaders
						/ (stem + "." + entryPoint + (glTarget ? ".gl.spv" : ".spv"));
					std::error_code copyEc;
					fs::copy_file(cachedPath, artifact, fs::copy_options::overwrite_existing, copyEc);
					if (copyEc)
					{
						++result.Failed;
						if (result.Error.empty())
							result.Error = "cannot write " + artifact.string() + ": " + copyEc.message();
						continue;
					}
					++result.Artifacts;
				}
			}
		}
		return result;
	}

	std::vector<char> ShaderCompiler::ReadBinaryFile(const std::string& filename)
	{
		std::vector<char> buffer = ReadAllBytes(filename);
		if (buffer.empty())
			WLD_CORE_ERROR("Failed to read shader binary: {0}", filename);
		return buffer;
	}
}
