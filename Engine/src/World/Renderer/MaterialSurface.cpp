#include "MaterialSurface.h"

#include "World/Core/Log.h"
#include "World/Renderer/ShaderUtils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace World
{
	namespace
	{
		namespace fs = std::filesystem;

		// 2 = M4-S2:包装源码加入注解参数块,并且每次编译都落一份反射输入供参数校验。
		// 3 = M4-S3:参数块挪到 b4/space1 + 顶点阶段(模板键缓存,不进 PS 键)。
		// 4 = Slang-T3:内核换成 slangc(双目标 SPIR-V + `-reflection-json` + 组合采样器模板)。
		// 5 = Slang-B1:生成的中间源码改名(`surface_user.hlsl` → `surface_user.slang`、
		//     `surface_wrapper.hlsl` / `vs_wrapper.hlsl` → `.slang`)—— 包装源码里的
		//     `#include` 名随之变化,键本来就含包装源码,升版让旧缓存目录自然作废。
		// 6 = Slang-B2-1:顶点实现收敛为泛型 `TransformVertex<T : IVertexSource>`(一个接口 +
		//     两个实现 + 三个同名特化入口);IO location / UBO / binding 与入口名逐项不变,
		//     模板文本变化 → 升版让旧产物失效重烘。
		constexpr uint32_t kSurfaceCacheVersion = 6;
		constexpr const char* kSurfaceEntryPoint = "PSMain";
		// 生成的中间用户源文件名:诊断里的 `File` 就是它,所以跟资产层同一口径用 `.slang`
		// (用户在编辑器里看到的是自己的 `.slang`/legacy `.hlsl`;这里只是编译脚手架)。
		constexpr const char* kUserSourceFileName = "surface_user.slang";
		constexpr const char* kReflectionFileName = "surface.reflection.json";

		// M4-S3(D5):表面模板自带的三个顶点入口。它们的输出(4 个插值量)与引擎
		// Renderer3D_Solid.slang 的不兼容,所以表面管线必须用模板自己的 VS。
		struct SurfaceVertexEntry
		{
			const char* EntryPoint;
			const char* FileStem;
		};

		constexpr SurfaceVertexEntry kSurfaceVertexEntries[] = {
			{ "VSMain", "vs_VSMain" },
			{ "VSMainInstanced", "vs_VSMainInstanced" },
			{ "VSMainSkinned", "vs_VSMainSkinned" },
		};

		std::atomic<size_t> s_CacheHits { 0 };
		std::atomic<size_t> s_CacheMisses { 0 };
		std::atomic<size_t> s_ToolInvocations { 0 };

		std::mutex s_CompileMutex;
		std::mutex s_LastGoodMutex;
		std::unordered_map<std::string, SurfaceArtifact> s_LastGood;

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

		std::string Trim(const std::string& text)
		{
			const size_t begin = text.find_first_not_of(" \t\r\n");
			if (begin == std::string::npos)
				return {};
			const size_t end = text.find_last_not_of(" \t\r\n");
			return text.substr(begin, end - begin + 1);
		}

		std::string SanitizeComment(const std::string& text)
		{
			std::string out = text;
			for (char& ch : out)
			{
				if (ch == '\r' || ch == '\n')
					ch = ' ';
			}
			return out;
		}

		std::string NormalizeSlashes(std::string text)
		{
			for (char& ch : text)
			{
				if (ch == '\\')
					ch = '/';
			}
			return text;
		}

		std::string LowerAscii(std::string text)
		{
			for (char& ch : text)
			{
				if (ch >= 'A' && ch <= 'Z')
					ch = static_cast<char>(ch - 'A' + 'a');
			}
			return text;
		}

		bool EndsWith(const std::string& text, const std::string& suffix)
		{
			return text.size() >= suffix.size() &&
				text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		bool ReadAllText(const fs::path& path, std::string& out)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return false;
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			out = buffer.str();
			return true;
		}

		bool ReadAllBytes(const fs::path& path, std::vector<uint8_t>& out)
		{
			std::ifstream stream(path, std::ios::ate | std::ios::binary);
			if (!stream)
				return false;
			const std::streamsize size = stream.tellg();
			if (size <= 0)
				return false;
			out.resize(static_cast<size_t>(size));
			stream.seekg(0);
			stream.read(reinterpret_cast<char*>(out.data()), size);
			return static_cast<bool>(stream);
		}

		bool WriteAllText(const fs::path& path, const std::string& text)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
				return false;
			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
			return static_cast<bool>(stream);
		}

		uint64_t FileSize(const std::string& toolPath)
		{
			std::error_code ec;
			const uint64_t size = static_cast<uint64_t>(fs::file_size(toolPath, ec));
			return ec ? 0 : size;
		}

		// ---- Slang 工具解析(Slang-T5:只剩一份实现) ----
		//
		// 工具目录是构建期决定的单一事实源(根 CMake 的 WLD_SLANG_DIR,可 -D 覆盖;
		// 落盘由 tools/agents/fetch-slang.ps1 负责)。这里直接用 ShaderUtils 里的
		// ShaderCompiler::SlangcPath() —— 渲染内核与烘焙/发行路径不再各有一份解析,
		// 也不再有 env/vendor/PATH 兜底猜测。
		const std::string& SlangcPath()
		{
			return ShaderCompiler::SlangcPath();
		}

		// 文件内容哈希(64 位 FNV-1a,分块读)—— 工具身份用它:换 Slang 构建即失效。
		uint64_t HashFileBytes(const std::string& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return 0;
			uint64_t hash = 14695981039346656037ULL;
			std::vector<char> buffer(1u << 16);
			while (stream)
			{
				stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const std::streamsize read = stream.gcount();
				for (std::streamsize index = 0; index < read; ++index)
				{
					hash ^= static_cast<uint8_t>(buffer[static_cast<size_t>(index)]);
					hash *= 1099511628211ULL;
				}
			}
			return hash;
		}

		// 工具身份:slangc.exe 与自己的编译器 DLL 的**大小 + 内容哈希**一起参与
		// (slang-compiler.dll 换代也必须让缓存失效)。只算一次 —— 哈希 60MB 级文件
		// 只发生在首次编译。
		uint64_t SlangToolIdentity()
		{
			static const uint64_t identity = []
			{
				const std::string tool = SlangcPath();
				if (tool.empty())
					return uint64_t(0);
				const fs::path toolPath(tool);
				uint64_t hash = FileSize(tool);
				hash = Mix(hash, std::to_string(FileSize(tool)));
				hash = Mix(hash, Hex(HashFileBytes(tool)));
				const fs::path compilerDll = toolPath.parent_path() / "slang-compiler.dll";
				std::error_code ec;
				if (fs::is_regular_file(compilerDll, ec))
				{
					const std::string dll = compilerDll.string();
					hash = Mix(hash, std::to_string(FileSize(dll)));
					hash = Mix(hash, Hex(HashFileBytes(dll)));
				}
				return hash;
			}();
			return identity;
		}

		std::string BackendKey(SurfaceShaderBackend backend)
		{
			switch (backend)
			{
				case SurfaceShaderBackend::VulkanSpirV: return "vulkan-spirv";
				case SurfaceShaderBackend::OpenGLSpirV: return "opengl-spirv";
			}
			return "unknown";
		}

		// 目标后端的 slangc profile(与 ShaderUtils.cpp 的 ShaderCompiler 同一口径):
		//  - Vulkan:直接用请求的 D3D profile(vs_6_0 / ps_6_0;产物 SPIR-V 1.3);
		//  - GL:`<stage>_5_0+spirv_1_0` —— ARB_gl_spirv 只接受 SPIR-V 1.0(T1 实测:
		//    默认 1.5 与 ps_6_0+spirv_1_0 的 1.3 都被 spirv-val --target-env opengl4.5 拒绝)。
		std::string ProfileForTarget(SurfaceShaderBackend backend, const std::string& d3dProfile)
		{
			if (backend != SurfaceShaderBackend::OpenGLSpirV)
				return d3dProfile;
			const size_t underscore = d3dProfile.find('_');
			const std::string stage = underscore == std::string::npos
				? d3dProfile : d3dProfile.substr(0, underscore);
			return stage + "_5_0+spirv_1_0";
		}

		// 一个 stage 的 slangc 命令行。两家目标的差异只有两处(与 T2 的引擎 shader 路径一致):
		//  - Vulkan:`-fvk-use-entrypoint-name` —— 模块入口名 = 源里的入口名(pipeline 的 pName 直接用);
		//  - GL:入口名保持 Slang 默认的 "main"(ARB_gl_spirv 的 glSpecializeShader 固定用它),
		//    并保留 T1 验证过的 `-fvk-use-gl-layout`。
		// `-reflection-json` 只有像素阶段要(参数反射的事实源;顶点阶段不带参数块)。
		std::string BuildSlangArguments(SurfaceShaderBackend backend, const std::string& d3dProfile,
			const std::string& entryPoint, const std::string& sourcePath, const std::string& outputPath,
			const std::string& reflectionPath)
		{
			std::string arguments = "-target spirv -profile \""
				+ ProfileForTarget(backend, d3dProfile) + "\" ";
			arguments += backend == SurfaceShaderBackend::OpenGLSpirV
				? "-fvk-use-gl-layout " : "-fvk-use-entrypoint-name ";
			arguments += "-entry \"" + entryPoint + "\" \"" + sourcePath + "\" -o \"" + outputPath + "\"";
			if (!reflectionPath.empty())
				arguments += " -reflection-json \"" + reflectionPath + "\"";
			return arguments;
		}

		std::string LastGoodKey(const std::string& permutationKey, SurfaceShaderBackend backend)
		{
			return BackendKey(backend) + "\n" + permutationKey;
		}

		void StoreLastGood(const std::string& permutationKey, SurfaceShaderBackend backend,
			const SurfaceArtifact& artifact)
		{
			std::lock_guard<std::mutex> lock(s_LastGoodMutex);
			s_LastGood[LastGoodKey(permutationKey, backend)] = artifact;
		}

#ifdef _WIN32
		bool RunToolCapture(const std::string& exe, const std::string& arguments, const std::string& logPath,
			int& exitCode, std::string& output)
		{
			SECURITY_ATTRIBUTES attributes {};
			attributes.nLength = sizeof(attributes);
			attributes.bInheritHandle = TRUE;

			HANDLE logHandle = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (logHandle == INVALID_HANDLE_VALUE)
			{
				exitCode = -1;
				return false;
			}
			HANDLE nulHandle = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
				&attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

			STARTUPINFOA startup {};
			startup.cb = sizeof(startup);
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdOutput = logHandle;
			startup.hStdError = logHandle;
			startup.hStdInput = (nulHandle == INVALID_HANDLE_VALUE) ? GetStdHandle(STD_INPUT_HANDLE) : nulHandle;

			PROCESS_INFORMATION process {};
			std::string commandLine = "\"" + exe + "\" " + arguments;
			const BOOL launched = CreateProcessA(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
				0, nullptr, nullptr, &startup, &process);
			if (nulHandle != INVALID_HANDLE_VALUE)
				CloseHandle(nulHandle);
			if (!launched)
			{
				CloseHandle(logHandle);
				exitCode = -1;
				return false;
			}

			WaitForSingleObject(process.hProcess, INFINITE);
			DWORD code = 0;
			GetExitCodeProcess(process.hProcess, &code);
			CloseHandle(process.hProcess);
			CloseHandle(process.hThread);
			CloseHandle(logHandle);

			exitCode = static_cast<int>(code);
			return ReadAllText(fs::path(logPath), output);
		}
#else
		bool RunToolCapture(const std::string& exe, const std::string& arguments, const std::string& logPath,
			int& exitCode, std::string& output)
		{
			const std::string commandLine = "\"" + exe + "\" " + arguments + " > \"" + logPath + "\" 2>&1";
			exitCode = std::system(commandLine.c_str());
			return ReadAllText(fs::path(logPath), output);
		}
#endif

		std::vector<SurfaceDiagnostic> ParseDiagnostics(const std::string& toolOutput)
		{
			std::vector<SurfaceDiagnostic> diagnostics;
			// Slang 的默认(rich)形态是**两行**:
			//   error[E20002]: syntax error
			//    --> <file>:<line>:<col>
			// 其后是源码片段与插入符行(跳过,不当成独立诊断)。
			// 单行 `<file>:<line>:<col>: <severity>: <msg>` 与 `<file>(<line>,<col>): ...` 作为兜底
			// 保留(Slang 的 -enable-machine-readable-diagnostics 形态与其它工具形态)。
			const std::regex header(R"(^\s*(error|warning)\s*(?:\[[A-Za-z0-9]+\])?\s*:\s*(.*)$)",
				std::regex::ECMAScript);
			const std::regex arrow(R"(^\s*-->\s*(.+?):(\d+):(\d+)\s*$)", std::regex::ECMAScript);
			const std::regex parenthesized(
				R"(^\s*(.+?)\((\d+),(\d+)\)\s*:\s*(error|warning)\s*:?\s*(.*)$)",
				std::regex::ECMAScript);
			const std::regex colonized(
				R"(^\s*(.+?):(\d+):(\d+)\s*:\s*(error|warning)\s*:?\s*(.*)$)",
				std::regex::ECMAScript);
			const std::regex snippet(R"(^(\s*[|^-].*|\s*\d+\s*\|.*)$)", std::regex::ECMAScript);

			const std::string userFile = LowerAscii(NormalizeSlashes(kUserSourceFileName));
			const auto markUserSource = [&userFile](SurfaceDiagnostic& diagnostic)
			{
				const std::string reported = LowerAscii(NormalizeSlashes(diagnostic.File));
				if (reported == userFile || EndsWith(reported, "/" + userFile))
				{
					diagnostic.InUserSource = true;
					diagnostic.UserLine = diagnostic.Line;
					diagnostic.UserColumn = diagnostic.Column;
				}
			};

			std::string pendingSeverity;
			std::string pendingMessage;
			std::istringstream stream(toolOutput);
			std::string line;
			while (std::getline(stream, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				const std::string trimmed = Trim(line);
				if (trimmed.empty())
					continue;

				std::smatch match;
				if (std::regex_match(line, match, header))
				{
					// 上一条 header 没等到 `-->`(工具只打了错误行)→ 先落一条无位置诊断。
					if (!pendingMessage.empty())
					{
						SurfaceDiagnostic diagnostic;
						diagnostic.Severity = pendingSeverity;
						diagnostic.Message = pendingMessage;
						diagnostics.push_back(std::move(diagnostic));
					}
					pendingSeverity = match[1].str();
					pendingMessage = trimmed;   // 保留 E 码:用户/编辑器能直接搜
					continue;
				}
				if (std::regex_match(line, match, arrow))
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Severity = pendingSeverity.empty() ? "error" : pendingSeverity;
					diagnostic.Message = trimmed;
					diagnostic.File = match[1].str();
					diagnostic.Line = static_cast<uint32_t>(std::stoul(match[2].str()));
					diagnostic.Column = static_cast<uint32_t>(std::stoul(match[3].str()));
					markUserSource(diagnostic);
					diagnostics.push_back(std::move(diagnostic));
					pendingSeverity.clear();
					pendingMessage.clear();
					continue;
				}
				if (std::regex_match(line, match, parenthesized) ||
					std::regex_match(line, match, colonized))
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Severity = match[4].str();
					diagnostic.Message = line;
					diagnostic.File = match[1].str();
					diagnostic.Line = static_cast<uint32_t>(std::stoul(match[2].str()));
					diagnostic.Column = static_cast<uint32_t>(std::stoul(match[3].str()));
					markUserSource(diagnostic);
					diagnostics.push_back(std::move(diagnostic));
					pendingSeverity.clear();
					pendingMessage.clear();
					continue;
				}
				if (std::regex_match(line, snippet))
					continue;
				if (pendingMessage.empty())
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Message = trimmed;
					diagnostic.Severity = (trimmed.find("error") != std::string::npos) ? "error" : "";
					diagnostics.push_back(std::move(diagnostic));
				}
			}
			if (!pendingMessage.empty())
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = pendingSeverity;
				diagnostic.Message = pendingMessage;
				diagnostics.push_back(std::move(diagnostic));
			}
			return diagnostics;
		}

		// Slang 版本串(`slangc -v`,只算一次;失败给空串)。与文件哈希一起构成缓存键里的
		// "Slang 工具身份" —— 版本能直接读出来,调试日志/报告里也用它。
		// 注意:这不是一次"编译调用",不计入 ToolInvocationCount。
		const std::string& SlangVersionText()
		{
			static const std::string version = []() -> std::string
			{
				const std::string tool = SlangcPath();
				if (tool.empty())
					return {};
				std::error_code ec;
				const fs::path logPath = fs::temp_directory_path(ec) / "we-slangc-version.log";
				if (ec)
					return {};
				int exitCode = -1;
				std::string output;
				if (!RunToolCapture(tool, "-v", logPath.string(), exitCode, output))
					return {};
				std::error_code removeEc;
				fs::remove(logPath, removeEc);
				return Trim(output);
			}();
			return version;
		}

		bool HasErrorDiagnostic(const std::vector<SurfaceDiagnostic>& diagnostics)
		{
			for (const SurfaceDiagnostic& diagnostic : diagnostics)
			{
				if (diagnostic.Severity == "error")
					return true;
			}
			return false;
		}

		std::string BuildDefaultSurfaceFunction()
		{
			std::string text;
			text += "// Engine standard surface: every Surface field gets a default assignment.\n";
			text += "// User code only changes the fields it cares about.\n";
			text += "Surface MakeDefaultSurface()\n{\n";
			text += "    Surface surface;\n";
			for (const MaterialSurfaceContract::FieldInfo& field : MaterialSurfaceContract::SurfaceFields)
			{
				text += "    surface.";
				text += field.Name;
				text += " = ";
				text += field.DefaultExpression;
				text += ";\n";
			}
			text += "    return surface;\n}\n";
			return text;
		}

		// 参数类型 → 参数块里的 HLSL 类型。注意 HLSL 的 bool 在 SPIR-V 里是 uint32
		// (反射校验按 uint 认它,见 MaterialParams.cpp 的 ReflectedTypeMatches)。
		const char* ParamHlslType(ParamType type)
		{
			switch (type)
			{
				case ParamType::Float: return "float";
				case ParamType::Vec2: return "float2";
				case ParamType::Vec3: return "float3";
				case ParamType::Vec4: return "float4";
				case ParamType::Color: return "float4";
				case ParamType::Int: return "int";
				case ParamType::Bool: return "bool";
				case ParamType::Texture2D: return "Texture2D";
			}
			return "float";
		}

		// 注解 → 参数块源码。标量/向量进 `cbuffer MaterialParams`(b4/space1;
		// 2 被 set0 的灯光 UBO 占用 —— GL 的 UBO 单元 = binding,忽略 set),
		// 贴图按注解顺序占 space2 的 t4、t5…,**每张一个组合采样器 `Sampler2D`**
		// (Slang-T3:ARB_gl_spirv 不接受分离的 OpTypeSampler,组合形态同时喂 Vulkan 的
		// COMBINED_IMAGE_SAMPLER 与 GL_SPIRV),数量上限 kMaxMaterialTextureSlots
		// (超出由编译入口给结构化错误)。
		// 布局(偏移/大小)不在这里手写:编译后用 Slang 的反射 JSON 读出来。
		std::string BuildParamBlockText(const std::vector<MaterialParamDecl>& params)
		{
			std::string members;
			std::string textures;
			uint32_t textureIndex = 0;
			for (const MaterialParamDecl& param : params)
			{
				if (IsTextureParamType(param.Type))
				{
					const uint32_t binding = ParamTextureBaseBinding() + textureIndex;
					++textureIndex;
					textures += "[[vk::binding(";
					textures += std::to_string(binding);
					textures += ", 2)]] Sampler2D ";
					textures += param.Name;
					textures += ";\n";
					continue;
				}
				members += "    ";
				members += ParamHlslType(param.Type);
				members += " ";
				members += param.Name;
				members += ";\n";
			}

			std::string text;
			text += "\n// ---- M4-S2 material parameters (generated from //! param annotations) ----\n";
			if (!members.empty())
			{
				text += "// 参数值由编辑器/运行时按反射出的偏移写入;这里只声明参数块。\n";
				text += "[[vk::binding(";
				text += std::to_string(ParamCbufferBinding());
				text += ", ";
				text += std::to_string(ParamCbufferSet());
				text += ")]] cbuffer ";
				text += ParamCbufferName();
				text += "\n{\n";
				text += members;
				text += "};\n";
			}
			if (!textures.empty())
			{
				text += "// 贴图参数(采样器与贴图同名 + Sampler 后缀):\n";
				text += textures;
			}
			return text;
		}

		// 引擎模板:顶点/光照/阴影/实例化/蒙皮/雾钩子由引擎提供。
		// 注意:雾目前只有一个恒等钩子 —— 当前全局 UBO 里没有雾参数,不伪造接口;
		// S2/S3 接入雾 uniform 时替换 ApplyEngineFog() 即可。
		const char* kSurfaceTemplatePrefix = R"WESURFACE(
// ============================================================================
// Engine wrapper below: vertex stage / lighting / shadows / instancing /
// skinning are provided by WorldEngine. Only Evaluate() comes from the user.
// ============================================================================

struct SurfaceVSInput
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float3 Normal : NORMAL;
    [[vk::location(2)]] float2 UV : TEXCOORD0;
};

struct SurfaceVSOutput
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 WorldNormal : NORMAL;
    [[vk::location(1)]] float3 WorldPosition : POSITION1;
    [[vk::location(2)]] float2 UV : TEXCOORD0;
    [[vk::location(3)]] nointerpolation float EntityId : ENTITYID;
};

// Slang-T6b:实例化入口必须用**一个**合并输入 struct(顶点属性 0..2 + per-instance 3..8)。
// Slang 不会把两个参数各自的 struct 按显式 `[[vk::location]]` 合并成一个接口 —— 第二个
// 参数会被顺移到 6..11:管线按 C++ 顶点布局声明 0..8,于是 Vulkan 报
// VUID-VkGraphicsPipelineCreateInfo-Input-07904(缺 Location 9/11),per-instance 矩阵
// 还会读到错位槽位(6/7/8 = Row3/Color/EntityId 被当成 Row0/Row1/Row2)。
// 引擎着色器 Renderer3D_Solid.slang 的 VS_INSTANCED_INPUT 是同一写法(实测有效)。
struct SurfaceInstancedInput
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float3 Normal : NORMAL;
    [[vk::location(2)]] float2 UV : TEXCOORD0;
    [[vk::location(3)]] float4 Row0 : INSTANCE0;
    [[vk::location(4)]] float4 Row1 : INSTANCE1;
    [[vk::location(5)]] float4 Row2 : INSTANCE2;
    [[vk::location(6)]] float4 Row3 : INSTANCE3;
    [[vk::location(7)]] float4 Color : INSTANCE4;
    [[vk::location(8)]] float4 EntityId : INSTANCE5;
};

struct SurfaceSkinnedInput
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float3 Normal : NORMAL;
    [[vk::location(2)]] float2 UV : TEXCOORD0;
    [[vk::location(3)]] float4 Joints : JOINTS;
    [[vk::location(4)]] float4 Weights : WEIGHTS;
};

struct SurfacePSOutput
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

[[vk::binding(0, 0)]] cbuffer CameraUniforms
{
    float4x4 u_ViewProjection;
};

struct GpuLight
{
    float4 PositionType;
    float4 ColorIntensity;
    float4 DirectionRange;
};

[[vk::binding(2, 0)]] cbuffer LightUniforms
{
    float4x4 u_ShadowViewProjection;
    float4 u_ShadowParams;
    float4 u_Ambient;
    uint4 u_LightCounts;
    GpuLight u_Lights[8];
};

[[vk::binding(3, 0)]] Sampler2D u_ShadowMap;

[[vk::binding(1, 1)]] cbuffer ObjectUniforms
{
    float4x4 u_Model;
    float4 u_BaseColor;
    float4 u_MetallicRoughness;
    float4 u_Emissive;
    float4 u_Flags;
    int4 u_EntityId;
};

[[vk::binding(3, 1)]] cbuffer BoneUniforms
{
    float4x4 u_Bones[128];
};

[[vk::binding(1, 2)]] Sampler2D u_AlbedoTexture;
[[vk::binding(2, 2)]] Sampler2D u_NormalTexture;

float3 WeLinearizeColor(float3 srgb)
{
    return pow(saturate(srgb), 2.2f);
}

float3 WeDefaultNormal()
{
    return float3(0.0f, 0.0f, 1.0f);
}

float4x4 ComputeSkinPalette(SurfaceSkinnedInput input)
{
    const int4 joints = (int4)round(input.Joints);
    float4x4 blended = (float4x4)0;
    [unroll] for (int index = 0; index < 4; ++index)
    {
        const uint boneIndex = (uint)clamp(joints[index], 0, 127);
        blended += u_Bones[boneIndex] * input.Weights[index];
    }
    return blended;
}

float SampleDirectionalShadow(float3 worldPosition, float3 normal, float3 toLightDirection)
{
    const float4 shadowPosition = mul(u_ShadowViewProjection, float4(worldPosition, 1.0f));
    if (shadowPosition.w <= 0.0f)
        return 1.0f;
    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float depth = (u_LightCounts.z > 0u) ? (projected.z * 0.5f + 0.5f) : projected.z;
    const float2 uv = projected.xy * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;
    const float cosTheta = saturate(dot(normal, toLightDirection));
    const float bias = max(u_ShadowParams.y * (1.0f - cosTheta), u_ShadowParams.y * 0.25f);
    const float2 texel = u_ShadowParams.w / max(u_ShadowParams.z, 1.0f);
    float visible = 0.0f;
    [unroll] for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll] for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            const float sampledDepth = u_ShadowMap.Sample(
                uv + float2(offsetX, offsetY) * texel).r;
            visible += (depth - bias <= sampledDepth) ? 1.0f : 0.0f;
        }
    }
    return visible / 9.0f;
}

SurfaceVSOutput BuildVSOutput(float4 worldPosition, float3 worldNormal, float2 uv, float entityId)
{
    SurfaceVSOutput output;
    output.Position = mul(u_ViewProjection, worldPosition);
    output.WorldNormal = worldNormal;
    output.WorldPosition = worldPosition.xyz;
    output.UV = uv;
    output.EntityId = entityId;
    return output;
}

// ---- Slang-B2-1(候选形态 D):无分支泛型 —— 变体差异全部落在两个实现里 ----
interface IVertexSource
{
    float3 Position();
    float3 Normal();
    float2 UV();
    float4x4 LocalPalette();
};

float4x4 WeIdentityMatrix()
{
    return float4x4(1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f);
}

struct PlainVertexSource : IVertexSource
{
    SurfaceVSInput Attributes;
    float3 Position() { return Attributes.Position; }
    float3 Normal() { return Attributes.Normal; }
    float2 UV() { return Attributes.UV; }
    float4x4 LocalPalette() { return WeIdentityMatrix(); }
};

struct SkinnedVertexSource : IVertexSource
{
    SurfaceSkinnedInput Attributes;
    float3 Position() { return Attributes.Position; }
    float3 Normal() { return Attributes.Normal; }
    float2 UV() { return Attributes.UV; }
    float4x4 LocalPalette()
    {
        // 原实现:权重全零、或蒙皮位置非有限 → 用未蒙皮的位置/法线。
        // 单位阵把这条回退编码进"调色板"本身 → 泛型实现里不再需要分支。
        if (!any(Attributes.Weights != 0.0f))
            return WeIdentityMatrix();
        const float4x4 blended = ComputeSkinPalette(Attributes);
        const float4 skinnedPosition = mul(blended, float4(Attributes.Position, 1.0f));
        return all(isfinite(skinnedPosition)) ? blended : WeIdentityMatrix();
    }
};

SurfaceVSOutput TransformVertex<T : IVertexSource>(T source, float4x4 model, float entityId)
{
    const float4x4 palette = source.LocalPalette();
    const float4 localPosition = mul(palette, float4(source.Position(), 1.0f));
    const float3 localNormal = mul((float3x3)palette, source.Normal());
    const float4 worldPosition = mul(model, localPosition);
    const float3x3 normalMatrix = (float3x3)transpose((float3x3)model);
    return BuildVSOutput(worldPosition, mul(normalMatrix, localNormal), source.UV(), entityId);
}

[shader("vertex")]
SurfaceVSOutput VSMain(SurfaceVSInput input)
{
    PlainVertexSource source;
    source.Attributes = input;
    return TransformVertex(source, u_Model, (float)u_EntityId.x);
}

[shader("vertex")]
SurfaceVSOutput VSMainInstanced(SurfaceInstancedInput input)
{
    PlainVertexSource source;
    source.Attributes.Position = input.Position;
    source.Attributes.Normal = input.Normal;
    source.Attributes.UV = input.UV;
    const float4x4 model = float4x4(input.Row0, input.Row1, input.Row2, input.Row3);
    return TransformVertex(source, model, input.EntityId.x);
}

[shader("vertex")]
SurfaceVSOutput VSMainSkinned(SurfaceSkinnedInput input)
{
    SkinnedVertexSource source;
    source.Attributes = input;
    return TransformVertex(source, u_Model, (float)u_EntityId.x);
}

MaterialInputs BuildMaterialInputs(SurfaceVSOutput input)
{
    MaterialInputs materialInputs;
    materialInputs.UV = input.UV;
    materialInputs.WorldPosition = input.WorldPosition;
    materialInputs.WorldNormal = normalize(input.WorldNormal);
    const float3 dpdx = ddx(input.WorldPosition);
    const float3 dpdy = ddy(input.WorldPosition);
    const float2 duvdx = ddx(input.UV);
    const float2 duvdy = ddy(input.UV);
    float3 tangent = dpdx * duvdy.y - dpdy * duvdx.y;
    if (dot(tangent, tangent) < 1e-12f)
    {
        const float3 axis = abs(materialInputs.WorldNormal.y) < 0.99f
            ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        tangent = cross(axis, materialInputs.WorldNormal);
    }
    materialInputs.WorldTangent = normalize(tangent);
    const float3 bitangent = cross(materialInputs.WorldNormal, materialInputs.WorldTangent);
    materialInputs.WorldBitangent = dot(bitangent, bitangent) > 1e-12f
        ? normalize(bitangent) : float3(0.0f, 0.0f, 0.0f);
    return materialInputs;
}

float3 ResolveShadingNormal(SurfaceVSOutput input, MaterialInputs materialInputs, Surface surface)
{
    float3 tangentNormal = float3(0.0f, 0.0f, 1.0f);
    if (u_Flags.y > 0.5f)
        tangentNormal = u_NormalTexture.Sample(input.UV).xyz * 2.0f - 1.0f;
    // Surface.Normal 是"在引擎采样结果之上的切空间扰动";默认 (0,0,1) 保持采样值。
    float3 combined = float3(tangentNormal.xy + surface.Normal.xy, surface.Normal.z);
    if (dot(combined, combined) < 1e-12f)
        combined = float3(0.0f, 0.0f, 1.0f);
    tangentNormal = normalize(combined);
    const float3 shading = materialInputs.WorldTangent * tangentNormal.x
        + materialInputs.WorldBitangent * tangentNormal.y
        + materialInputs.WorldNormal * tangentNormal.z;
    const float shadingLengthSquared = dot(shading, shading);
    return shadingLengthSquared > 1e-12f ? shading * rsqrt(shadingLengthSquared)
        : materialInputs.WorldNormal;
}

float3 EvaluateEngineLighting(MaterialInputs materialInputs, Surface surface, float3 shadingNormal)
{
    float3 albedo = max(surface.BaseColor, 0.0f);
    if (u_Flags.x > 0.5f)
        albedo *= u_AlbedoTexture.Sample(materialInputs.UV).rgb;

    const float metallic = saturate(surface.Metallic);
    const float roughness = saturate(surface.Roughness);
    const float3 viewDirection = float3(0.0f, 0.0f, 1.0f);
    float3 litColor = albedo * u_Ambient.rgb * u_Ambient.a * saturate(surface.AmbientOcclusion);

    const uint lightCount = min(u_LightCounts.x + u_LightCounts.y, 8u);
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        const GpuLight light = u_Lights[lightIndex];
        const bool isDirectional = light.PositionType.w > 0.5f;
        float3 toLight;
        float attenuation = 1.0f;
        if (isDirectional)
        {
            toLight = -normalize(light.DirectionRange.xyz);
        }
        else
        {
            const float3 offset = light.PositionType.xyz - materialInputs.WorldPosition;
            const float distanceToLight = length(offset);
            toLight = distanceToLight > 1e-5f ? offset / distanceToLight : float3(0.0f, 0.0f, 0.0f);
            const float falloff = saturate(1.0f - distanceToLight / max(light.DirectionRange.x, 1e-4f));
            attenuation = falloff * falloff;
        }

        const float lambert = saturate(dot(shadingNormal, toLight));
        float visibility = 1.0f;
        if (isDirectional && u_ShadowParams.x > 0.5f)
            visibility = SampleDirectionalShadow(materialInputs.WorldPosition, shadingNormal, toLight);

        const float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a * attenuation * visibility;
        const float diffuse = lambert * lerp(1.0f, 0.35f, metallic);
        const float3 halfVector = normalize(toLight + viewDirection + 1e-5f);
        const float specularPower = lerp(8.0f, 128.0f, saturate(1.0f - roughness));
        const float specular = lambert > 0.0f
            ? pow(saturate(dot(shadingNormal, halfVector)), specularPower) * lerp(0.04f, 1.0f, metallic)
            : 0.0f;
        litColor += albedo * radiance * diffuse + radiance * specular;
    }

    litColor += max(surface.Emissive, 0.0f);
    return litColor;
}

// 雾钩子:当前全局 UBO 没有雾参数,引擎也没有雾通道,所以这里保持恒等。
// S2/S3 接入 u_FogParams/u_FogColor 后,只替换这一个函数即可。
float3 ApplyEngineFog(float3 color, MaterialInputs materialInputs)
{
    return color;
}
)WESURFACE";

		const char* kSurfaceTemplateSuffix = R"WESURFACE(
SurfacePSOutput PSMain(SurfaceVSOutput input)
{
    SurfacePSOutput output;
    const MaterialInputs materialInputs = BuildMaterialInputs(input);
    Surface surface = Evaluate(materialInputs);
    const float3 shadingNormal = ResolveShadingNormal(input, materialInputs, surface);
    float3 litColor = EvaluateEngineLighting(materialInputs, surface, shadingNormal);
    litColor = ApplyEngineFog(litColor, materialInputs);
    output.Color = float4(pow(saturate(litColor), 1.0f / 2.2f), saturate(surface.Opacity));
    output.EntityID = (int)round(input.EntityId);
    return output;
}
)WESURFACE";

		std::string BuildWrapperSource(const std::string& contractText, const std::string& userSource,
			const std::string& permutationKey, SurfaceShaderBackend backend,
			const std::vector<MaterialParamDecl>& params)
		{
			std::string wrapper;
			wrapper.reserve(contractText.size() + userSource.size() + 16384);
			wrapper += "// WorldEngine M4-S1 generated material surface wrapper. Do not edit.\n";
			wrapper += "// backend: ";
			wrapper += BackendKey(backend);
			wrapper += "\n// permutation: ";
			wrapper += SanitizeComment(permutationKey);
			wrapper += "\n\n// ---- MaterialSurfaceContract.hlsli (embedded) ----\n";
			wrapper += contractText;
			if (wrapper.empty() || wrapper.back() != '\n')
				wrapper += '\n';
			wrapper += kSurfaceTemplatePrefix;
			wrapper += "\n// ---- generated default surface (from the shared field table) ----\n";
			wrapper += BuildDefaultSurfaceFunction();
			wrapper += BuildParamBlockText(params);
			wrapper += "\n// ---- user surface function ----\n#include \"";
			wrapper += kUserSourceFileName;
			wrapper += "\"\n";
			wrapper += kSurfaceTemplateSuffix;
			return wrapper;
		}

		// M4-S3(D5):顶点阶段的编译与缓存。
		//
		// 关键决定:VS 包装里塞的是**引擎默认表面函数**,不是用户源 —— 模板自带的
		// VSMain/VSMainInstanced/VSMainSkinned 都不引用 Evaluate(),所以 VS 的编译输入
		// 只由"包装模板 + 参数块 + 排列键 + 工具/契约身份"决定。缓存键因此**不含用户源**:
		// 改用户代码时只有 PSMain 需要真的跑编译器(稳态下每次编辑一次),首帧之后几乎全命中。
		//
		// 失败语义:任一个顶点入口编译失败 = 本次编译整体失败(结构化诊断),由调用方决定
		// 是否继续用上一份已发布管线。缺文件(缓存被清)时**重新编译该入口**,不静默少阶段。
		std::string BuildVertexWrapperSource(const std::string& contractText, const std::string& permutationKey,
			SurfaceShaderBackend backend, const std::vector<MaterialParamDecl>& params)
		{
			return BuildWrapperSource(contractText, MaterialSurfaceCompiler::DefaultSurfaceFunctionSource(),
				permutationKey, backend, params);
		}

		bool ResolveVertexStages(const std::string& contractText,
			const std::vector<MaterialParamDecl>& params, const std::string& permutationKey,
			SurfaceShaderBackend backend, std::vector<SurfaceVertexStage>* out,
			std::vector<SurfaceDiagnostic>* diagnostics, std::string& rawToolOutput)
		{
			if (!out)
				return false;
			out->clear();

			const std::string wrapperSource = BuildVertexWrapperSource(contractText, permutationKey,
				backend, params);
			uint64_t keyHash = Fnv1a64String(wrapperSource);
			keyHash = Mix(keyHash, BackendKey(backend));
			keyHash = Mix(keyHash, permutationKey);
			keyHash = Mix(keyHash, std::to_string(kSurfaceCacheVersion));
			keyHash = Mix(keyHash, Hex(SlangToolIdentity()));
			keyHash = Mix(keyHash, SlangVersionText());
			const std::string keyHex = Hex(keyHash);

			const fs::path cacheRoot = fs::path(WLD_INTERMEDIATE_DIR) / "SurfaceShaderCache";
			const fs::path keyDir = cacheRoot / ("vs-" + keyHex);
			const std::string slangc = SlangcPath();

			std::error_code ec;
			fs::create_directories(keyDir, ec);
			if (ec)
			{
				if (diagnostics)
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Severity = "error";
					diagnostic.Message = "cannot create surface vertex cache dir: " + ec.message();
					diagnostics->push_back(std::move(diagnostic));
				}
				return false;
			}

			const fs::path wrapperPath = keyDir / "vs_wrapper.slang";
			const fs::path logPath = keyDir / "vs_compile.log";
			// 包装源码用 `#include "surface_user.slang"` 引用用户源;VS 这一份固定写
			// 引擎默认表面函数(VS 不调用 Evaluate())—— 键里因此不含用户源。
			const fs::path userPath = keyDir / kUserSourceFileName;
			if (!WriteAllText(userPath, MaterialSurfaceCompiler::DefaultSurfaceFunctionSource())
				|| !WriteAllText(wrapperPath, wrapperSource))
			{
				if (diagnostics)
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Severity = "error";
					diagnostic.Message = "cannot write generated surface vertex source under "
						+ keyDir.string();
					diagnostics->push_back(std::move(diagnostic));
				}
				return false;
			}

			for (const SurfaceVertexEntry& entry : kSurfaceVertexEntries)
			{
				const fs::path spvPath = keyDir / (std::string(entry.FileStem) + ".spv");
				std::vector<uint8_t> bytecode;
				if (fs::is_regular_file(spvPath, ec) && fs::file_size(spvPath, ec) > 0
					&& ReadAllBytes(spvPath, bytecode) && !bytecode.empty())
				{
					SurfaceVertexStage stage;
					stage.EntryPoint = entry.EntryPoint;
					stage.Bytecode = std::move(bytecode);
					out->push_back(std::move(stage));
					continue;
				}

				fs::remove(spvPath, ec);
				const std::string arguments = BuildSlangArguments(backend, "vs_6_0",
					entry.EntryPoint, wrapperPath.string(), spvPath.string(), std::string());
				int exitCode = -1;
				std::string toolOutput;
				s_ToolInvocations.fetch_add(1, std::memory_order_relaxed);
				const bool launched = !slangc.empty()
					&& RunToolCapture(slangc, arguments, logPath.string(), exitCode,
					toolOutput);
				rawToolOutput += toolOutput;
				if (!toolOutput.empty() && toolOutput.back() != '\n')
					rawToolOutput += '\n';
				if (diagnostics)
				{
					const std::vector<SurfaceDiagnostic> parsed = ParseDiagnostics(toolOutput);
					diagnostics->insert(diagnostics->end(), parsed.begin(), parsed.end());
				}

				const bool ready = launched && exitCode == 0 && fs::is_regular_file(spvPath, ec)
					&& fs::file_size(spvPath, ec) > 0 && ReadAllBytes(spvPath, bytecode)
					&& !bytecode.empty();
				if (!ready)
				{
					fs::remove(spvPath, ec);
					if (diagnostics)
					{
						SurfaceDiagnostic diagnostic;
						diagnostic.Severity = "error";
						diagnostic.Message = !launched
							? ("cannot launch slangc for vertex entry " + std::string(entry.EntryPoint)
								+ ": " + (slangc.empty()
									? std::string("slangc.exe not found (set WLD_SLANG_DIR)")
									: slangc))
							: ("surface vertex entry " + std::string(entry.EntryPoint)
								+ " failed to compile (exit code " + std::to_string(exitCode) + ")");
						diagnostics->push_back(std::move(diagnostic));
					}
					out->clear();
					return false;
				}

				SurfaceVertexStage stage;
				stage.EntryPoint = entry.EntryPoint;
				stage.Bytecode = std::move(bytecode);
				out->push_back(std::move(stage));
			}
			return true;
		}
	}

	const char* MaterialSurfaceCompiler::BackendName(SurfaceShaderBackend backend)
	{
		switch (backend)
		{
			case SurfaceShaderBackend::VulkanSpirV: return "vulkan-spirv";
			case SurfaceShaderBackend::OpenGLSpirV: return "opengl-spirv";
		}
		return "unknown";
	}

	std::string MaterialSurfaceCompiler::ContractHeaderPath()
	{
		return std::string(WLD_WORLD_DIR) + "src/World/Renderer/MaterialSurfaceContract.hlsli";
	}

	std::string MaterialSurfaceCompiler::DefaultSurfaceFunctionSource()
	{
		return
			"// Engine standard surface. Change only the fields this material needs.\n"
			"// MaterialInputs is filled by the engine; Surface defaults come from the\n"
			"// engine's standard shader and stay complete even if you change nothing.\n"
			"Surface Evaluate(MaterialInputs input)\n"
			"{\n"
			"    Surface surface = MakeDefaultSurface();\n"
			"    // Examples:\n"
			"    // surface.BaseColor = float3(1.0f, 0.35f, 0.2f);\n"
			"    // surface.Roughness = 0.25f;\n"
			"    // surface.Normal = float3(0.0f, 0.0f, 1.0f);\n"
			"    return surface;\n"
			"}\n";
	}

	std::string MaterialSurfaceCompiler::WrapSurfaceSource(const std::string& userSource,
		SurfaceShaderBackend backend)
	{
		// 注解坏了时只丢参数块(这里是诊断用的"看包装源码"入口),编译入口会返回结构化错误。
		std::vector<MaterialParamDecl> params;
		std::string parseError;
		if (!ParseMaterialParams(userSource, &params, &parseError))
			params.clear();
		return WrapSurfaceSourceWithParams(userSource, params, backend);
	}

	std::string MaterialSurfaceCompiler::WrapSurfaceSourceWithParams(const std::string& userSource,
		const std::vector<MaterialParamDecl>& params, SurfaceShaderBackend backend)
	{
		std::string contractText;
		if (!ReadAllText(fs::path(ContractHeaderPath()), contractText))
			return {};
		const std::string effectiveSource = Trim(userSource).empty()
			? DefaultSurfaceFunctionSource() : userSource;
		return BuildWrapperSource(contractText, effectiveSource, "", backend, params);
	}

	std::string MaterialSurfaceCompiler::BuildParamBlockSource(const std::vector<MaterialParamDecl>& params)
	{
		return BuildParamBlockText(params);
	}

	std::string MaterialSurfaceCompiler::ReflectionPath(const SurfaceArtifact& artifact)
	{
		if (artifact.CacheKey.empty())
			return {};
		const fs::path path = fs::path(WLD_INTERMEDIATE_DIR) / "SurfaceShaderCache"
			/ artifact.CacheKey / kReflectionFileName;
		std::error_code ec;
		if (!fs::is_regular_file(path, ec))
			return {};
		const uintmax_t size = fs::file_size(path, ec);
		if (ec || size == 0)
			return {};
		return path.string();
	}

	size_t MaterialSurfaceCompiler::CacheHitCount()
	{
		return s_CacheHits.load(std::memory_order_relaxed);
	}

	size_t MaterialSurfaceCompiler::CacheMissCount()
	{
		return s_CacheMisses.load(std::memory_order_relaxed);
	}

	size_t MaterialSurfaceCompiler::ToolInvocationCount()
	{
		return s_ToolInvocations.load(std::memory_order_relaxed);
	}

	void MaterialSurfaceCompiler::ResetCounters()
	{
		s_CacheHits.store(0, std::memory_order_relaxed);
		s_CacheMisses.store(0, std::memory_order_relaxed);
		s_ToolInvocations.store(0, std::memory_order_relaxed);
	}

	void MaterialSurfaceCompiler::ClearLastGood()
	{
		std::lock_guard<std::mutex> lock(s_LastGoodMutex);
		s_LastGood.clear();
	}

	bool MaterialSurfaceCompiler::LastGood(const std::string& permutationKey, SurfaceShaderBackend backend,
		SurfaceArtifact& out)
	{
		std::lock_guard<std::mutex> lock(s_LastGoodMutex);
		const auto found = s_LastGood.find(LastGoodKey(permutationKey, backend));
		if (found == s_LastGood.end())
			return false;
		out = found->second;
		return true;
	}

	SurfaceCompileResult MaterialSurfaceCompiler::CompileSurface(const std::string& source,
		const std::string& permutationKey, SurfaceShaderBackend backend)
	{
		// M4-S2:注解是参数的事实源。解析失败 = 结构化诊断(带用户源行列号),不调用编译器。
		std::vector<MaterialParamDecl> params;
		std::string parseError;
		if (!ParseMaterialParams(source, &params, &parseError))
		{
			SurfaceCompileResult result;
			SurfaceDiagnostic diagnostic;
			diagnostic.Severity = "error";
			diagnostic.Message = "material param annotation: " + parseError;
			diagnostic.File = kUserSourceFileName;
			diagnostic.InUserSource = true;
			// 解析错误串是 `<行>:<列>: <原因>`;行列拿给编辑器定位。
			const size_t firstColon = parseError.find(':');
			const size_t secondColon = firstColon == std::string::npos ? std::string::npos
				: parseError.find(':', firstColon + 1);
			if (firstColon != std::string::npos && secondColon != std::string::npos)
			{
				try
				{
					diagnostic.UserLine = static_cast<uint32_t>(std::stoul(parseError.substr(0, firstColon)));
					diagnostic.UserColumn = static_cast<uint32_t>(
						std::stoul(parseError.substr(firstColon + 1, secondColon - firstColon - 1)));
					diagnostic.Line = diagnostic.UserLine;
					diagnostic.Column = diagnostic.UserColumn;
				}
				catch (const std::exception&)
				{
					diagnostic.UserLine = 0;
					diagnostic.UserColumn = 0;
				}
			}
			result.Diagnostics.push_back(std::move(diagnostic));
			SurfaceArtifact ignoredLastGood;
			result.LastGoodAvailable = LastGood(permutationKey, backend, ignoredLastGood);
			return result;
		}
		return CompileSurfaceWithParams(source, params, permutationKey, backend);
	}

	SurfaceCompileResult MaterialSurfaceCompiler::CompileSurfaceWithParams(const std::string& source,
		const std::vector<MaterialParamDecl>& params, const std::string& permutationKey,
		SurfaceShaderBackend backend)
	{
		SurfaceCompileResult result;
		const auto started = std::chrono::steady_clock::now();
		const auto finish = [&result, started]()
		{
			result.ElapsedMilliseconds =
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			return result;
		};

		try
		{
			SurfaceArtifact ignoredLastGood;
			result.LastGoodAvailable = LastGood(permutationKey, backend, ignoredLastGood);

			// Slang-T3:两家目标都由 slangc 编成 SPIR-V —— Vulkan 用 Vulkan profile,
			// GL 用 `<stage>_5_0+spirv_1_0`(SPIR-V 1.0;GL 侧运行时接入归 M4-S4)。
			if (backend != SurfaceShaderBackend::VulkanSpirV
				&& backend != SurfaceShaderBackend::OpenGLSpirV)
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.Message = "surface shader backend '" + std::string(BackendName(backend)) +
					"' is not a known Slang target (expected vulkan-spirv or opengl-spirv)";
				result.Diagnostics.push_back(std::move(diagnostic));
				return finish();
			}

			// 参数表的轻量自检(调用方可能直接给手工表):名字合法/唯一/不与引擎保留名冲突。
			{
				std::unordered_map<std::string, bool> seen;
				uint32_t textureCount = 0;
				for (const MaterialParamDecl& param : params)
				{
					const bool valid = IsUsableParamName(param.Name);
					if (!valid || seen.count(param.Name) != 0)
					{
						SurfaceDiagnostic diagnostic;
						diagnostic.Severity = "error";
						diagnostic.Message = valid
							? "material param '" + param.Name + "' is declared twice"
							: "material param name '" + param.Name + "' is not a usable HLSL identifier "
								"(reserved engine name or invalid characters)";
						result.Diagnostics.push_back(std::move(diagnostic));
						return finish();
					}
					seen.emplace(param.Name, true);
					// M4-S3:贴图槽位是固定的一段(space2 的 t4..t11),超出上限给结构化错误 ——
					// 让运行时的描述符槽位与声明永远对得上,不静默丢参数。
					if (IsTextureParamType(param.Type) && ++textureCount > kMaxMaterialTextureSlots)
					{
						SurfaceDiagnostic diagnostic;
						diagnostic.Severity = "error";
						diagnostic.Message = "material param '" + param.Name + "': too many Texture2D params ("
							+ std::to_string(textureCount) + "), the parameter block allows at most "
							+ std::to_string(kMaxMaterialTextureSlots) + " (t"
							+ std::to_string(ParamTextureBaseBinding()) + "..t"
							+ std::to_string(ParamTextureBaseBinding() + kMaxMaterialTextureSlots - 1)
							+ ", space2)";
						result.Diagnostics.push_back(std::move(diagnostic));
						return finish();
					}
				}
			}

			const std::string effectiveSource = Trim(source).empty()
				? DefaultSurfaceFunctionSource() : source;

			std::string contractText;
			if (!ReadAllText(fs::path(ContractHeaderPath()), contractText))
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.Message = "surface contract header missing: " + ContractHeaderPath();
				result.Diagnostics.push_back(std::move(diagnostic));
				return finish();
			}

			const std::string wrapperSource = BuildWrapperSource(contractText, effectiveSource,
				permutationKey, backend, params);
			const std::string paramBlockSource = BuildParamBlockText(params);
			const uint64_t sourceHash = Fnv1a64String(effectiveSource);
			uint64_t keyHash = Fnv1a64String(wrapperSource);
			// 包装源码把用户源作为 #include 引用,所以用户源内容必须显式进键;
			// 否则"只改用户代码"会错误命中同一份 artifact(实测 2026-09-22)。
			keyHash = Mix(keyHash, Hex(sourceHash));
			// Slang-T3:注解表(参数块文本)与契约版本也显式进键 —— 只改注解(不动用户源)
			// 或只改契约头文件,都必须换一份 artifact。
			keyHash = Mix(keyHash, Hex(Fnv1a64String(paramBlockSource)));
			keyHash = Mix(keyHash, Hex(Fnv1a64String(contractText)));
			keyHash = Mix(keyHash, BackendKey(backend));
			keyHash = Mix(keyHash, permutationKey);
			keyHash = Mix(keyHash, std::to_string(kSurfaceCacheVersion));
			// Slang 工具身份 = 版本/二进制大小 + 内容哈希(slangc.exe + slang-compiler.dll)。
			keyHash = Mix(keyHash, Hex(SlangToolIdentity()));
			keyHash = Mix(keyHash, SlangVersionText());
			const std::string keyHex = Hex(keyHash);

			std::lock_guard<std::mutex> lock(s_CompileMutex);

			const fs::path cacheRoot = fs::path(WLD_INTERMEDIATE_DIR) / "SurfaceShaderCache";
			const fs::path keyDir = cacheRoot / keyHex;
			const fs::path spvPath = keyDir / "surface.spv";
			const fs::path reflectionPath = keyDir / kReflectionFileName;

			std::error_code ec;
			// 反射 JSON(-reflection-json)是参数校验/上传的输入:产物在但反射丢了 →
			// 当成未命中,重新编译补上(旧缓存的产物没有这份文件,自然全部作废)。
			if (fs::is_regular_file(spvPath, ec) && fs::file_size(spvPath, ec) > 0
				&& fs::is_regular_file(reflectionPath, ec) && fs::file_size(reflectionPath, ec) > 0)
			{
				std::vector<uint8_t> cached;
				if (ReadAllBytes(spvPath, cached) && !cached.empty())
				{
					result.Artifact.Bytecode = std::move(cached);
					result.Artifact.CacheKey = keyHex;
					result.Artifact.Backend = BackendName(backend);
					result.Artifact.EntryPoint = kSurfaceEntryPoint;
					result.Artifact.SourceHash = sourceHash;
					// M4-S3:缓存命中同样要把顶点阶段(模板键)读回 artifact;VS 缓存缺失时
					// 这里会补一次编译。
					if (!ResolveVertexStages(contractText, params, permutationKey, backend,
						&result.Artifact.VertexStages, &result.Diagnostics, result.RawToolOutput))
					{
						result.Success = false;
						return finish();
					}
					s_CacheHits.fetch_add(1, std::memory_order_relaxed);
					result.Success = true;
					result.CacheHit = true;
					StoreLastGood(permutationKey, backend, result.Artifact);
					return finish();
				}
			}

			s_CacheMisses.fetch_add(1, std::memory_order_relaxed);
			fs::create_directories(keyDir, ec);
			if (ec)
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.Message = "cannot create surface cache dir: " + ec.message();
				result.Diagnostics.push_back(std::move(diagnostic));
				return finish();
			}

			const fs::path userPath = keyDir / kUserSourceFileName;
			const fs::path wrapperPath = keyDir / "surface_wrapper.slang";
			const fs::path logPath = keyDir / "surface_compile.log";
			if (!WriteAllText(userPath, effectiveSource) || !WriteAllText(wrapperPath, wrapperSource))
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.Message = "cannot write generated surface source under " + keyDir.string();
				result.Diagnostics.push_back(std::move(diagnostic));
				return finish();
			}

			fs::remove(spvPath, ec);
			fs::remove(reflectionPath, ec);
			const std::string slangc = SlangcPath();
			// 参数反射(MaterialParams.cpp)直接读这份 `-reflection-json`(不再解析 -Fc 汇编文本);
			// profile/入口名/布局开关按目标后端分(Slang-T3,T1/T2 已实测)。
			const std::string arguments = BuildSlangArguments(backend, "ps_6_0",
				kSurfaceEntryPoint, wrapperPath.string(), spvPath.string(), reflectionPath.string());
			int exitCode = -1;
			std::string toolOutput;
			s_ToolInvocations.fetch_add(1, std::memory_order_relaxed);
			const bool launched = !slangc.empty()
				&& RunToolCapture(slangc, arguments, logPath.string(), exitCode, toolOutput);
			result.RawToolOutput = toolOutput;
			result.Diagnostics = ParseDiagnostics(toolOutput);

			std::vector<uint8_t> bytecode;
			const bool artifactReady = launched && exitCode == 0 &&
				fs::is_regular_file(spvPath, ec) && fs::file_size(spvPath, ec) > 0 &&
				ReadAllBytes(spvPath, bytecode) && !bytecode.empty();

			if (!artifactReady)
			{
				if (!launched)
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Severity = "error";
					diagnostic.Message = slangc.empty()
						? "cannot launch slangc: slangc.exe not found (set WLD_SLANG_DIR, or run the "
							"vendor/tools/slang FETCH)"
						: "cannot launch slangc: " + slangc;
					result.Diagnostics.push_back(std::move(diagnostic));
					result.RawToolOutput += "\n" + result.Diagnostics.back().Message + "\n";
				}
				else if (!HasErrorDiagnostic(result.Diagnostics))
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Severity = "error";
					diagnostic.Message = toolOutput.empty()
						? "slangc failed with exit code " + std::to_string(exitCode)
						: toolOutput;
					result.Diagnostics.push_back(std::move(diagnostic));
				}
				fs::remove(spvPath, ec);
				return finish();
			}

			result.Success = true;
			result.Artifact.Bytecode = std::move(bytecode);
			result.Artifact.CacheKey = keyHex;
			result.Artifact.Backend = BackendName(backend);
			result.Artifact.EntryPoint = kSurfaceEntryPoint;
			result.Artifact.SourceHash = sourceHash;
			// M4-S3(D5):PS 成功后编译/读取模板自带的三个顶点阶段(按模板键缓存)。
			// 任一个失败 → 本次编译整体失败(VS 与 PS 必须成对)。
			if (!ResolveVertexStages(contractText, params, permutationKey, backend,
				&result.Artifact.VertexStages, &result.Diagnostics, result.RawToolOutput))
			{
				result.Success = false;
				return finish();
			}
			StoreLastGood(permutationKey, backend, result.Artifact);
			return finish();
		}
		catch (const std::exception& error)
		{
			SurfaceDiagnostic diagnostic;
			diagnostic.Severity = "error";
			diagnostic.Message = std::string("surface compile threw: ") + error.what();
			result.Diagnostics.push_back(std::move(diagnostic));
			result.Success = false;
			return finish();
		}
		catch (...)
		{
			SurfaceDiagnostic diagnostic;
			diagnostic.Severity = "error";
			diagnostic.Message = "surface compile threw an unknown exception";
			result.Diagnostics.push_back(std::move(diagnostic));
			result.Success = false;
			return finish();
		}
	}
}
