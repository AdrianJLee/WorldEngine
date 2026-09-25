#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/RendererAPI.h"
#include "World/Renderer/MaterialSurface.h"
#include "World/Renderer/ShaderUtils.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	struct TempDir
	{
		std::filesystem::path path;

		TempDir()
		{
			std::error_code ec;
			const uint64_t unique =
				static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			path = std::filesystem::temp_directory_path(ec) / ("we-shader-" + std::to_string(unique));
			std::filesystem::create_directories(path, ec);
			if (ec)
				throw std::runtime_error("failed to create temp dir: " + ec.message());
		}

		~TempDir()
		{
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

	void WriteText(const std::filesystem::path& path, const std::string& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
			throw std::runtime_error("failed to write " + path.string());
		stream << text;
	}

	// Slang-T5:工具目录只有一个入口 —— 构建期 `WLD_SLANG_DIR` 决定的
	// `ShaderCompiler::SlangcPath()`。测试里**不再**自带一份 env/vendor/同级目录扫描:
	// "去哪儿找工具"由配置决定一次,测试只问引擎要结论(与内核/烘焙同一条路径)。
	bool HasSlangc()
	{
		return !World::ShaderCompiler::SlangcPath().empty();
	}

	// ---- 产物形态判据(与引擎的 SpirvIsGlIngestable 同一套逐字节口径)----
	//
	// ARB_gl_spirv 只接受 SPIR-V 1.0,且不接受分离采样器 OpTypeSampler
	// (T1 实测:这种模块交给 glShaderBinary 后 NVIDIA 驱动在第一次采样时崩,14/14)。
	std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream)
			return {};
		const std::streamsize size = stream.tellg();
		if (size <= 0)
			return {};
		stream.seekg(0);
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		stream.read(reinterpret_cast<char*>(bytes.data()), size);
		return bytes;
	}

	bool SpirvLooksValid(const std::vector<uint8_t>& bytes, uint32_t* version)
	{
		if (bytes.size() < 20 || (bytes.size() % 4) != 0)
			return false;
		uint32_t magic = 0;
		std::memcpy(&magic, bytes.data(), sizeof(magic));
		if (magic != 0x07230203u)
			return false;
		if (version)
			std::memcpy(version, bytes.data() + 4, sizeof(*version));
		return true;
	}

	bool SpirvHasSeparateSamplerType(const std::vector<uint8_t>& bytes)
	{
		if (!SpirvLooksValid(bytes, nullptr))
			return false;
		const size_t words = bytes.size() / 4;
		for (size_t offset = 5; offset < words;)
		{
			uint32_t word = 0;
			std::memcpy(&word, bytes.data() + offset * 4, sizeof(word));
			const uint32_t wordCount = word >> 16;
			if (wordCount == 0)
				break;
			if ((word & 0xFFFFu) == 26u)   // OpTypeSampler
				return true;
			offset += wordCount;
		}
		return false;
	}

	const char* kProbeShader = R"(
struct VS_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
};

struct VS_OUTPUT
{
    float4 Position : SV_Position;
};

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    output.Position = float4(input.a_Position, 1.0f);
    return output;
}

float4 PSMain(VS_OUTPUT input) : SV_Target0
{
    return float4(1.0f, 0.5f, 0.25f, 1.0f);
}
)";
}

int main()
{
	World::Log::Init();
	try
	{
		// 1. 产物命名规则(打包与运行时共用)
		{
			CHECK(World::ShaderCompiler::ArtifactLogicalPath("assets/shaders/Foo.slang", "VSMain", true) ==
				"shaders/Foo.VSMain.spv");
			// Slang-T6b:GL 目标的产物也是 SPIR-V(`.gl.` 中缀),没有 GLSL 文本产物了。
			CHECK(World::ShaderCompiler::ArtifactLogicalPath("assets/shaders/Foo.slang", "VSMain", false) ==
				"shaders/Foo.VSMain.gl.spv");
			// Slang-T6a:表面材质产物的命名**只有一处实现**(EditorCooker 写、运行时读):
			//   shaders/surface/<内容根相对路径去扩展名>.<入口>[.gl].spv | .PSMain[.gl].reflection.json
			// (`.gl.` 中缀 = GL 4.6 + ARB_gl_spirv 的 SPIR-V 1.0 目标;两者都是 SPIR-V。)
			CHECK(World::MaterialLibrary::SurfaceArtifactBasePath("shaders/Glass.slang") ==
				"shaders/surface/shaders/Glass");
			CHECK(World::MaterialLibrary::SurfaceArtifactLogicalPath("shaders/Glass.slang", "PSMain", false) ==
				"shaders/surface/shaders/Glass.PSMain.spv");
			CHECK(World::MaterialLibrary::SurfaceArtifactLogicalPath("shaders/Glass.slang", "VSMainSkinned", true) ==
				"shaders/surface/shaders/Glass.VSMainSkinned.gl.spv");
			CHECK(World::MaterialLibrary::SurfaceReflectionLogicalPath("shaders/Glass.slang", false) ==
				"shaders/surface/shaders/Glass.PSMain.reflection.json");
			CHECK(World::MaterialLibrary::SurfaceReflectionLogicalPath("fx/sub/Glass.slang", true) ==
				"shaders/surface/fx/sub/Glass.PSMain.gl.reflection.json");
			// 只去掉**最后一段**扩展名(目录里的点、文件名里的多点都不动)。
			CHECK(World::MaterialLibrary::SurfaceArtifactBasePath("fx/v1.2/Glass.surface.slang") ==
				"shaders/surface/fx/v1.2/Glass.surface");
		}

		// 2. 解析器优先:有烘焙产物时不触发工具调用(发行形态路径)
		{
			// Slang-T6b:GL 目标也必须是在**活设备**上才解析(capability 门),所以这段决议
			// 路径按 Vulkan 目标跑(`.spv` 与设备无关);GL 目标由打包探针在真后端上覆盖。
			const World::RendererAPI::API previousApi = World::Renderer::GetAPI();
			World::RendererAPI::SetAPI(World::RendererAPI::API::Vulkan);
			World::ShaderCompiler::ClearArtifactResolver();
			World::ShaderCompiler::ResetCounters();
			World::ShaderCompiler::SetArtifactResolver([](const std::string& logical, std::vector<uint8_t>& out)
			{
				if (logical.rfind("shaders/", 0) != 0)
					return false;
				out = { 0xDE, 0xAD, 0xBE, 0xEF };
				return true;
			});
			const std::vector<char> bytes =
				World::ShaderCompiler::CompileOrLoad("assets/shaders/Missing.slang", "VSMain", "vs_6_0");
			CHECK(bytes.size() == 4);
			CHECK(static_cast<unsigned char>(bytes[0]) == 0xDE);
			CHECK(World::ShaderCompiler::CookedHitCount() == 1);
			CHECK(World::ShaderCompiler::ToolInvocationCount() == 0);
			World::ShaderCompiler::ClearArtifactResolver();
			World::RendererAPI::SetAPI(previousApi);
		}

		if (!HasSlangc())
		{
			std::printf("World.ShaderPipeline: slangc missing (WLD_SLANG_DIR), bake checks skipped\n");
			std::printf("World.ShaderPipeline: all checks passed\n");
			return 0;
		}

		// 3. M4-S1:表面函数契约 + 引擎包装模板 + 编译/缓存/失败回退
		TempDir temp;
		const std::filesystem::path sourceDir = temp.path / "assets" / "shaders";
		const std::filesystem::path cookedDir = temp.path / "cooked";
		{
			using World::MaterialSurfaceCompiler;
			using World::SurfaceArtifact;
			using World::SurfaceShaderBackend;

			const std::string runTag = temp.path.filename().string();
			const std::string minimalSource = "Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.Roughness = saturate(0.5f + input.UV.x * 0.0f);\n"
				"    return surface;\n"
				"}\n"
				"// m4s1 minimal " + runTag + "\n";
			const std::string syntaxErrorSource = "Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.Roughness = ;\n"
				"    return surface;\n"
				"}\n"
				"// m4s1 syntax " + runTag + "\n";
			const std::string undeclaredSource = "Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.BaseColor = missingParameter;\n"
				"    return surface;\n"
				"}\n"
				"// m4s1 undeclared " + runTag + "\n";

			// ① 最小合法表面函数 → 编译成功 + artifact 非空 + 重复编译逐字节一致。
			MaterialSurfaceCompiler::ResetCounters();
			const World::SurfaceCompileResult minimalFirst =
				MaterialSurfaceCompiler::CompileSurface(minimalSource, "m4s1-minimal");
			CHECK(minimalFirst.Success);
			CHECK(!minimalFirst.CacheHit);
			CHECK(!minimalFirst.Artifact.Bytecode.empty());
			CHECK(minimalFirst.Artifact.Bytecode.size() >= 4);
			CHECK(minimalFirst.Artifact.Bytecode[0] == 0x03);
			CHECK(minimalFirst.Artifact.Bytecode[1] == 0x02);
			CHECK(minimalFirst.Artifact.Bytecode[2] == 0x23);
			CHECK(minimalFirst.Artifact.Bytecode[3] == 0x07);
			CHECK(minimalFirst.Artifact.EntryPoint == "PSMain");
			CHECK(minimalFirst.Artifact.Backend == "vulkan-spirv");
			// Slang-T3:Vulkan 目标由 slangc 出 SPIR-V(1.3);GL 目标必须是 1.0(asserted 下面)。
			{
				uint32_t spirvVersion = 0;
				CHECK(minimalFirst.Artifact.Bytecode.size() >= 8);
				std::memcpy(&spirvVersion, minimalFirst.Artifact.Bytecode.data() + 4, sizeof(spirvVersion));
				CHECK(spirvVersion >= 0x00010000u);
			}

			// ② 同一源第二次编译 → 缓存命中、不重编。
			const size_t toolRunsAfterMinimal = MaterialSurfaceCompiler::ToolInvocationCount();
			const size_t hitsAfterMinimal = MaterialSurfaceCompiler::CacheHitCount();
			const World::SurfaceCompileResult minimalSecond =
				MaterialSurfaceCompiler::CompileSurface(minimalSource, "m4s1-minimal");
			CHECK(minimalSecond.Success);
			CHECK(minimalSecond.CacheHit);
			CHECK(MaterialSurfaceCompiler::ToolInvocationCount() == toolRunsAfterMinimal);
			CHECK(MaterialSurfaceCompiler::CacheHitCount() == hitsAfterMinimal + 1);
			CHECK(minimalSecond.Artifact.Bytecode == minimalFirst.Artifact.Bytecode);

			// ①b 删掉缓存文件强制走一次真实编译(slangc),再验证工具输出逐字节确定。
			const std::filesystem::path cachedSpv =
				std::filesystem::path(WLD_INTERMEDIATE_DIR) / "SurfaceShaderCache" /
				minimalFirst.Artifact.CacheKey / "surface.spv";
			std::error_code removeEc;
			CHECK(std::filesystem::remove(cachedSpv, removeEc));
			const World::SurfaceCompileResult minimalRecompiled =
				MaterialSurfaceCompiler::CompileSurface(minimalSource, "m4s1-minimal");
			CHECK(minimalRecompiled.Success);
			CHECK(!minimalRecompiled.CacheHit);
			CHECK(minimalRecompiled.Artifact.Bytecode == minimalFirst.Artifact.Bytecode);

			// ①c M4-S3(D5):顶点阶段齐全(模板自带的三个入口)+ **按模板键缓存** ——
			// 改用户代码时只有 PSMain 真的跑编译器(稳态下每次编辑一次),VS 全部命中缓存。
			CHECK(minimalFirst.Artifact.VertexStages.size() == 3);
			// SurfaceVertexStage 没有 operator==(公开头冻结):按入口名 + 字节逐条比较。
			const auto sameVertexStages = [](const std::vector<World::SurfaceVertexStage>& left,
				const std::vector<World::SurfaceVertexStage>& right)
			{
				if (left.size() != right.size())
					return false;
				for (size_t index = 0; index < left.size(); ++index)
				{
					if (left[index].EntryPoint != right[index].EntryPoint)
						return false;
					if (left[index].Bytecode != right[index].Bytecode)
						return false;
				}
				return true;
			};
			const World::SurfaceVertexStage* vsMain = minimalFirst.Artifact.FindVertexStage("VSMain");
			const World::SurfaceVertexStage* vsInstanced =
				minimalFirst.Artifact.FindVertexStage("VSMainInstanced");
			const World::SurfaceVertexStage* vsSkinned =
				minimalFirst.Artifact.FindVertexStage("VSMainSkinned");
			CHECK(vsMain != nullptr && vsInstanced != nullptr && vsSkinned != nullptr);
			CHECK(minimalFirst.Artifact.FindVertexStage("PSMain") == nullptr);   // 顶点阶段里没有 PS 入口
			CHECK(vsMain->Bytecode.size() >= 4 && vsMain->Bytecode[0] == 0x03 && vsMain->Bytecode[3] == 0x07);
			// 删掉 PS 缓存后重编:VS 仍从模板键缓存读回,逐字节一致。
			CHECK(sameVertexStages(minimalRecompiled.Artifact.VertexStages, minimalFirst.Artifact.VertexStages));
			{
				const std::string vsCachePermutation = "m4s3-vs-cache-" + runTag;
				// 语义改动(不是注释):PS 字节必须真的不同,否则"VS 命中缓存"就没有说服力。
				std::string editedSource = minimalSource;
				const size_t roughnessAt = editedSource.find("0.5f");
				CHECK(roughnessAt != std::string::npos);
				editedSource.replace(roughnessAt, 4, "0.25f");
				const size_t toolsBeforeVsCache = MaterialSurfaceCompiler::ToolInvocationCount();
				const World::SurfaceCompileResult vsCacheFirst =
					MaterialSurfaceCompiler::CompileSurface(minimalSource, vsCachePermutation);
				CHECK(vsCacheFirst.Success);
				const size_t toolsAfterFirstVsCache = MaterialSurfaceCompiler::ToolInvocationCount();
				// 新排列键:1 次 PS + 3 次 VS(首次建立模板键缓存)。
				CHECK(toolsAfterFirstVsCache == toolsBeforeVsCache + 4);
				const World::SurfaceCompileResult vsCacheSecond =
					MaterialSurfaceCompiler::CompileSurface(editedSource, vsCachePermutation);
				CHECK(vsCacheSecond.Success);
				// 同一排列键、换了用户源:只有 PS 重编一次(VS 不含用户源 → 命中)。
				CHECK(MaterialSurfaceCompiler::ToolInvocationCount() == toolsAfterFirstVsCache + 1);
				CHECK(sameVertexStages(vsCacheSecond.Artifact.VertexStages, vsCacheFirst.Artifact.VertexStages));
				CHECK(vsCacheSecond.Artifact.Bytecode != vsCacheFirst.Artifact.Bytecode);
				std::printf("World.ShaderPipeline: M4-S3 vertex stages=%zu (compiler runs: first=%zu, edited=%zu)\n",
					vsCacheSecond.Artifact.VertexStages.size(),
					toolsAfterFirstVsCache - toolsBeforeVsCache,
					MaterialSurfaceCompiler::ToolInvocationCount() - toolsAfterFirstVsCache);
			}

			// ③ 语法错误 → 失败、错误消息含用户源行号(实测字符串打印在下面)。
			const World::SurfaceCompileResult syntaxFailure =
				MaterialSurfaceCompiler::CompileSurface(syntaxErrorSource, "m4s1-syntax");
			CHECK(!syntaxFailure.Success);
			bool syntaxMappedToUserLine4 = false;
			for (const World::SurfaceDiagnostic& diagnostic : syntaxFailure.Diagnostics)
			{
				if (diagnostic.Severity == "error" && diagnostic.InUserSource && diagnostic.UserLine == 4)
					syntaxMappedToUserLine4 = true;
			}
			CHECK(syntaxMappedToUserLine4);
			CHECK(syntaxFailure.RawToolOutput.find("error") != std::string::npos);

			// ④ 引用未声明标识符 → 失败 + 可读诊断。
			const World::SurfaceCompileResult undeclaredFailure =
				MaterialSurfaceCompiler::CompileSurface(undeclaredSource, "m4s1-undeclared");
			CHECK(!undeclaredFailure.Success);
			CHECK(undeclaredFailure.RawToolOutput.find("missingParameter") != std::string::npos);
			bool undeclaredMappedToUserLine4 = false;
			for (const World::SurfaceDiagnostic& diagnostic : undeclaredFailure.Diagnostics)
			{
				if (diagnostic.Severity == "error" && diagnostic.InUserSource && diagnostic.UserLine == 4)
					undeclaredMappedToUserLine4 = true;
			}
			CHECK(undeclaredMappedToUserLine4);

			// ⑤ 引擎模板自带默认表面函数(空源)→ 编译成功,证明模板本身可编译。
			CHECK(MaterialSurfaceCompiler::DefaultSurfaceFunctionSource().find("MakeDefaultSurface")
				!= std::string::npos);
			const World::SurfaceCompileResult defaultSurfaceCached =
				MaterialSurfaceCompiler::CompileSurface("", "m4s1-default");
			CHECK(defaultSurfaceCached.Success);
			const std::filesystem::path defaultSpv =
				std::filesystem::path(WLD_INTERMEDIATE_DIR) / "SurfaceShaderCache" /
				defaultSurfaceCached.Artifact.CacheKey / "surface.spv";
			CHECK(std::filesystem::remove(defaultSpv, removeEc));
			const World::SurfaceCompileResult defaultSurface =
				MaterialSurfaceCompiler::CompileSurface("", "m4s1-default");
			CHECK(defaultSurface.Success);
			CHECK(!defaultSurface.CacheHit);
			CHECK(!defaultSurface.Artifact.Bytecode.empty());

			// ⑥ 源改动一个字符 → 缓存键变化、未命中。
			const World::SurfaceCompileResult keyBaseline =
				MaterialSurfaceCompiler::CompileSurface(minimalSource, "m4s1-key");
			CHECK(keyBaseline.Success);
			std::string changedSource = minimalSource;
			const size_t roughnessValue = changedSource.find("0.5f");
			CHECK(roughnessValue != std::string::npos);
			changedSource.replace(roughnessValue, 4, "0.6f");
			CHECK(changedSource != minimalSource);
			const size_t missesBeforeChangedSource = MaterialSurfaceCompiler::CacheMissCount();
			const World::SurfaceCompileResult changedKey =
				MaterialSurfaceCompiler::CompileSurface(changedSource, "m4s1-key");
			CHECK(changedKey.Success);
			CHECK(!changedKey.CacheHit);
			CHECK(changedKey.Artifact.CacheKey != keyBaseline.Artifact.CacheKey);
			CHECK(MaterialSurfaceCompiler::CacheMissCount() == missesBeforeChangedSource + 1);

			// 失败回退语义:编译器不静默返回旧 artifact,调用方用 LastGood() 自己保留旧管线。
			const std::string lastGoodPermutation = "m4s1-lastgood";
			const World::SurfaceCompileResult goodForFallback =
				MaterialSurfaceCompiler::CompileSurface(minimalSource, lastGoodPermutation);
			CHECK(goodForFallback.Success);
			const World::SurfaceCompileResult badForFallback =
				MaterialSurfaceCompiler::CompileSurface(syntaxErrorSource, lastGoodPermutation);
			CHECK(!badForFallback.Success);
			CHECK(badForFallback.LastGoodAvailable);
			SurfaceArtifact lastGood;
			CHECK(MaterialSurfaceCompiler::LastGood(lastGoodPermutation,
				SurfaceShaderBackend::VulkanSpirV, lastGood));
			CHECK(lastGood.Bytecode == goodForFallback.Artifact.Bytecode);

			// Slang-T3:GL 目标也由 slangc 编成 SPIR-V(`<stage>_5_0+spirv_1_0`),产物**必须**是
			// SPIR-V 1.0(ARB_gl_spirv 的硬要求);GL 侧的运行时接入归 M4-S4,本层只保证产物对。
			{
				const size_t toolRunsBeforeOpenGL = MaterialSurfaceCompiler::ToolInvocationCount();
				// 排列键带本次运行的唯一 tag:否则第二次跑同一个测试时 GL 顶点阶段会命中上一轮的
				// 模板键缓存,工具调用次数就不再是 4(计数断言必须与缓存状态无关)。
				const std::string openGlPermutation = "m4s1-opengl-" + runTag;
				const World::SurfaceCompileResult openGlTarget =
					MaterialSurfaceCompiler::CompileSurface(minimalSource, openGlPermutation,
						SurfaceShaderBackend::OpenGLSpirV);
				CHECK(openGlTarget.Success);
				CHECK(!openGlTarget.CacheHit);
				CHECK(openGlTarget.Artifact.Backend == "opengl-spirv");
				CHECK(openGlTarget.Artifact.EntryPoint == "PSMain");
				CHECK(openGlTarget.Artifact.Bytecode.size() >= 8);
				CHECK(openGlTarget.Artifact.Bytecode[0] == 0x03 && openGlTarget.Artifact.Bytecode[1] == 0x02
					&& openGlTarget.Artifact.Bytecode[2] == 0x23 && openGlTarget.Artifact.Bytecode[3] == 0x07);
				uint32_t glVersion = 0;
				std::memcpy(&glVersion, openGlTarget.Artifact.Bytecode.data() + 4, sizeof(glVersion));
				CHECK(glVersion == 0x00010000u);   // ARB_gl_spirv:只接受 SPIR-V 1.0
				CHECK(openGlTarget.Artifact.VertexStages.size() == 3);
				// 目标不同 → 产物与缓存键都不同(Vulkan 那份是 SPIR-V 1.3)。
				CHECK(openGlTarget.Artifact.CacheKey != minimalFirst.Artifact.CacheKey);
				CHECK(openGlTarget.Artifact.Bytecode != minimalFirst.Artifact.Bytecode);
				CHECK(MaterialSurfaceCompiler::ToolInvocationCount() == toolRunsBeforeOpenGL + 4);
				// 同源同目标第二次 → 命中缓存,不再调用工具。
				const World::SurfaceCompileResult openGlCached =
					MaterialSurfaceCompiler::CompileSurface(minimalSource, openGlPermutation,
						SurfaceShaderBackend::OpenGLSpirV);
				CHECK(openGlCached.Success && openGlCached.CacheHit);
				CHECK(openGlCached.Artifact.Bytecode == openGlTarget.Artifact.Bytecode);
				CHECK(MaterialSurfaceCompiler::ToolInvocationCount() == toolRunsBeforeOpenGL + 4);
			}

			std::printf("World.ShaderPipeline: M4-S1 cache hits=%zu misses=%zu tool=%zu\n",
				MaterialSurfaceCompiler::CacheHitCount(),
				MaterialSurfaceCompiler::CacheMissCount(),
				MaterialSurfaceCompiler::ToolInvocationCount());
			std::printf("World.ShaderPipeline: M4-S1 keys first=%s changed=%s\n",
				minimalFirst.Artifact.CacheKey.c_str(), changedKey.Artifact.CacheKey.c_str());
			std::printf("World.ShaderPipeline: M4-S1 timing first=%.1fms hit=%.1fms recompile=%.1fms "
				"syntax=%.1fms undeclared=%.1fms default=%.1fms changed=%.1fms\n",
				minimalFirst.ElapsedMilliseconds, minimalSecond.ElapsedMilliseconds,
				minimalRecompiled.ElapsedMilliseconds, syntaxFailure.ElapsedMilliseconds,
				undeclaredFailure.ElapsedMilliseconds, defaultSurface.ElapsedMilliseconds,
				changedKey.ElapsedMilliseconds);
			uint32_t undeclaredUserLine = 0;
			uint32_t undeclaredUserColumn = 0;
			for (const World::SurfaceDiagnostic& diagnostic : undeclaredFailure.Diagnostics)
			{
				if (diagnostic.InUserSource)
				{
					undeclaredUserLine = diagnostic.UserLine;
					undeclaredUserColumn = diagnostic.UserColumn;
					break;
				}
			}
			std::printf("World.ShaderPipeline: M4-S1 syntax raw=%.320s\n",
				syntaxFailure.RawToolOutput.c_str());
			std::printf("World.ShaderPipeline: M4-S1 undeclared user line=%u col=%u\n",
				undeclaredUserLine, undeclaredUserColumn);
		}

		// 4. 烘焙(双目标 SPIR-V)→ 内容寻址缓存 → 打包 → 包内读回 → 运行时解析器命中
		//
		// Slang-T5/T6a 的产物契约:运行时真的会请求的每个入口烘**两份 SPIR-V** ——
		//   shaders/<stem>.<Entry>.spv      Vulkan 目标;
		//   shaders/<stem>.<Entry>.gl.spv   GL 4.6 + ARB_gl_spirv 目标(SPIR-V 1.0 + 组合采样器)。
		// Slang-T6b:全程只有 SPIR-V 产物(GLSL 文本路径已删除)。
		// 每次运行内容不同 → 指纹不同,保证真的走一次 slangc;第二次同一内容必须命中缓存。
		WriteText(sourceDir / "Probe.slang",
			std::string(kProbeShader) + "\n// build " + temp.path.filename().string() + "\n");

		World::ShaderCompiler::ResetCounters();
		const World::ShaderCompiler::BakeResult baked =
			World::ShaderCompiler::BakeDistributionTargets(sourceDir, cookedDir);
		CHECK(baked.Shaders == 1);
		CHECK(baked.Artifacts == 4);
		CHECK(baked.Failed == 0);
		const size_t toolRuns = World::ShaderCompiler::ToolInvocationCount();
		CHECK(toolRuns >= 2);

		const std::filesystem::path shaderOut = cookedDir / "shaders";
		for (const char* name : { "Probe.VSMain.spv", "Probe.VSMain.gl.spv",
			"Probe.PSMain.spv", "Probe.PSMain.gl.spv" })
		{
			std::error_code sizeEc;
			CHECK(std::filesystem::is_regular_file(shaderOut / name, sizeEc));
			CHECK(std::filesystem::file_size(shaderOut / name, sizeEc) > 0);
		}
		// Slang-T6b:烘焙产物里**没有** GLSL 文本(旧的 SPIR-V→GLSL 转译兜底已删除)——
		// 两个后端的产物都是 SPIR-V;这一条是"GLSL 路径归零"的可复跑判据。
		{
			std::error_code globEc;
			size_t glslCount = 0;
			for (const std::filesystem::directory_entry& entry :
				std::filesystem::directory_iterator(shaderOut, globEc))
			{
				if (entry.is_regular_file(globEc) && entry.path().extension() == ".glsl")
					++glslCount;
			}
			CHECK(glslCount == 0);
		}

		// GL 目标必须是 GL 4.6 能摄入的形态(逐字节判据,不是"文件存在就算过"):
		// SPIR-V 1.0(ARB_gl_spirv 的硬要求)+ 不含 OpTypeSampler(驱动会崩的分离采样器形态)。
		// Vulkan 目标则必须是**另一份**产物(不是同一份复制)。
		const std::vector<uint8_t> glVertexFirst = ReadFileBytes(shaderOut / "Probe.VSMain.gl.spv");
		{
			const std::vector<uint8_t> glPixel = ReadFileBytes(shaderOut / "Probe.PSMain.gl.spv");
			const std::vector<uint8_t> vulkanVertex = ReadFileBytes(shaderOut / "Probe.VSMain.spv");
			uint32_t glVersion = 0;
			uint32_t vulkanVersion = 0;
			CHECK(SpirvLooksValid(glVertexFirst, &glVersion));
			CHECK(SpirvLooksValid(glPixel, &glVersion));
			CHECK(glVersion == 0x00010000u);
			CHECK(!SpirvHasSeparateSamplerType(glVertexFirst));
			CHECK(!SpirvHasSeparateSamplerType(glPixel));
			CHECK(SpirvLooksValid(vulkanVertex, &vulkanVersion));
			CHECK(vulkanVersion != 0x00010000u);
			CHECK(vulkanVertex != glVertexFirst);
		}

		// 同一内容再次烘焙:命中缓存,工具调用次数不增长,产物逐字节不变。
		const World::ShaderCompiler::BakeResult again =
			World::ShaderCompiler::BakeDistributionTargets(sourceDir, cookedDir);
		CHECK(again.Artifacts == 4 && again.Failed == 0);
		CHECK(World::ShaderCompiler::ToolInvocationCount() == toolRuns);
		CHECK(World::ShaderCompiler::CacheHitCount() >= 2);
		CHECK(ReadFileBytes(shaderOut / "Probe.VSMain.gl.spv") == glVertexFirst);

		// 4b. Slang-B1sk:烘焙扫描只认 `.slang` —— 同一份内容换成 `.hlsl` 扩展名必须
		//     **被忽略**(0 源 / 0 产物 / 0 失败),`.hlsl` 不再是可烘焙的源。
		{
			const std::filesystem::path legacySourceDir = temp.path / "hlsl-ext-src" / "shaders";
			const std::filesystem::path legacyCookedDir = temp.path / "hlsl-ext-cooked";
			WriteText(legacySourceDir / "Legacy.hlsl",
				std::string(kProbeShader) + "\n// .hlsl extension fixture " + temp.path.filename().string() + "\n");
			World::ShaderCompiler::ResetCounters();
			const World::ShaderCompiler::BakeResult legacyBaked =
				World::ShaderCompiler::BakeDistributionTargets(legacySourceDir, legacyCookedDir);
			CHECK(legacyBaked.Shaders == 0);
			CHECK(legacyBaked.Artifacts == 0);
			CHECK(legacyBaked.Failed == 0);
			CHECK(World::ShaderCompiler::ToolInvocationCount() == 0);
			CHECK(!std::filesystem::exists(legacyCookedDir / "shaders" / "Legacy.VSMain.spv"));
			std::printf("World.ShaderPipeline: Slang-B1sk .hlsl ignored by the .slang-only bake scan\n");
		}

		// 打包为 wpak(与 Editor cook 的产物目录一致)并从包内读回。
		const std::filesystem::path pakPath = temp.path / "content.wpak";
		std::error_code pakEc;
		CHECK(World::Vfs::PackageProvider::BuildFromDirectory(cookedDir, pakPath, pakEc));
		std::shared_ptr<World::Vfs::PackageProvider> provider =
			World::Vfs::PackageProvider::Open(pakPath, pakEc);
		CHECK(provider != nullptr);

		std::vector<uint8_t> expected;
		CHECK(provider->Open("shaders/Probe.VSMain.spv", expected, pakEc));
		CHECK(!expected.empty());
		std::vector<uint8_t> expectedGl;
		CHECK(provider->Open("shaders/Probe.VSMain.gl.spv", expectedGl, pakEc));
		CHECK(!expectedGl.empty());

		// Runtime 侧:注册 VFS 解析器后,着色器不再走源码树编译(发行形态 = 只读产物)。
		// 这一段显式按 Vulkan 目标跑:`.spv` 与设备无关,不需要真设备;
		// GL 目标的摄入要走活设备的 GL_ARB_gl_spirv,由打包探针在真后端上覆盖。
		const World::RendererAPI::API previousApi = World::Renderer::GetAPI();
		World::RendererAPI::SetAPI(World::RendererAPI::API::Vulkan);
		World::ShaderCompiler::ResetCounters();
		World::ShaderCompiler::SetArtifactResolver(
			[&provider](const std::string& logical, std::vector<uint8_t>& out)
			{
				std::error_code readEc;
				return provider->Open(logical, out, readEc) && !out.empty();
			});
		const std::vector<char> packaged =
			World::ShaderCompiler::CompileOrLoad("assets/shaders/Probe.slang", "VSMain", "vs_6_0");
		CHECK(packaged.size() == expected.size());
		CHECK(World::ShaderCompiler::CookedHitCount() == 1);
		CHECK(World::ShaderCompiler::ToolInvocationCount() == 0);
		World::ShaderCompiler::ClearArtifactResolver();
		World::RendererAPI::SetAPI(previousApi);

		// 3b. M4-S2:注解参数表 → 参数块编译 → Slang 反射校验(三态)+ 字段偏移表 + 打包
		{
			using World::MaterialParamDecl;
			using World::MaterialParamLayout;
			using World::MaterialParamOverride;
			using World::MaterialSurfaceCompiler;
			using World::ParamType;

			const std::string annotated =
				"//! param Float Roughness = 0.4 [0,1] group(\"Surface\") label(\"Roughness\")\n"
				"//! param Color Tint = 1, 0.5, 0.25, 1 group(\"Surface\")\n"
				"//! param Bool Glow = true\n"
				"//! param Texture2D Albedo = \"textures/Icon.png\"\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.Roughness = Roughness;\n"
				"    surface.BaseColor = Tint.rgb * Albedo.Sample(input.UV).rgb;\n"
				"    if (Glow) surface.Emissive = float3(0.05f, 0.05f, 0.05f);\n"
				"    return surface;\n"
				"}\n";

			std::vector<MaterialParamDecl> table;
			std::string error;
			CHECK(World::ParseMaterialParams(annotated, &table, &error));
			CHECK(table.size() == 4);

			// ⑥ 与 M4-S1 编译管线串起来:注解生成的参数块真的能编译通过。
			const World::SurfaceCompileResult compiled =
				MaterialSurfaceCompiler::CompileSurface(annotated, "m4s2-annotated");
			CHECK(compiled.Success);
			CHECK(!compiled.Artifact.Bytecode.empty());
			CHECK(compiled.Artifact.EntryPoint == "PSMain");
			const std::string wrapper = MaterialSurfaceCompiler::WrapSurfaceSource(annotated,
				World::SurfaceShaderBackend::VulkanSpirV);
			// M4-S3(D1):参数块从 space1 b2 挪到 b4(GL 的 UBO 单元 = binding,单元 2 是灯光);
			// Slang-T3:cbuffer / 贴图都改用显式 `[[vk::binding]]`,贴图是**组合采样器** Sampler2D
			// (ARB_gl_spirv 不接受分离的 OpTypeSampler;同时喂 Vulkan 的 COMBINED_IMAGE_SAMPLER)。
			CHECK(wrapper.find("[[vk::binding(4, 1)]] cbuffer MaterialParams") != std::string::npos);
			CHECK(wrapper.find("float Roughness;") != std::string::npos);
			CHECK(wrapper.find("float4 Tint;") != std::string::npos);
			CHECK(wrapper.find("bool Glow;") != std::string::npos);
			CHECK(wrapper.find("[[vk::binding(4, 2)]] Sampler2D Albedo;") != std::string::npos);
			CHECK(wrapper.find("Texture2D Albedo") == std::string::npos);
			CHECK(wrapper.find("SamplerState AlbedoSampler") == std::string::npos);

			// 反射布局:名称/类型/偏移/大小来自 Slang 的反射 JSON(引擎不手写参数结构体)。
			MaterialParamLayout layout;
			CHECK(World::BuildParamLayout(annotated, table, &layout, &error));
			std::printf("World.ShaderPipeline: M4-S2 layout\n%s", World::FormatParamLayout(layout).c_str());
			// M4-S3(D1):参数块 = set 1 / binding 4(反射结果与生成器同一份约定)。
			CHECK(layout.CbufferSet == 1 && layout.CbufferBinding == 4);
			CHECK(layout.Fields.size() == 3);
			CHECK(layout.Fields[0].Name == "Roughness" && layout.Fields[0].Type == ParamType::Float);
			CHECK(layout.Fields[1].Name == "Tint" && layout.Fields[1].Type == ParamType::Color);
			CHECK(layout.Fields[2].Name == "Glow" && layout.Fields[2].Type == ParamType::Bool);
			CHECK(layout.Fields[0].Offset == 0);
			CHECK(layout.Fields[0].Size == 4);
			CHECK(layout.Fields[1].Offset >= layout.Fields[0].Offset + layout.Fields[0].Size);
			CHECK(layout.Fields[2].Offset >= layout.Fields[1].Offset + layout.Fields[1].Size);
			CHECK(layout.CbufferSize >= layout.Fields[2].Offset + layout.Fields[2].Size);
			CHECK(layout.CbufferSize % 16 == 0);
			CHECK(layout.Textures.size() == 1);
			CHECK(layout.Textures[0].Name == "Albedo");
			CHECK(layout.Textures[0].Set == 2 && layout.Textures[0].Binding == 4);
			CHECK(layout.UsedMembers.size() == 3);

			// Slang-T6a:反射 JSON 是**运行时上传**的事实源(打包形态没有编译器,靠的就是这份
			// 与产物同键的 JSON)。直接按 artifact 的 JSON + SPIR-V 反射出的布局必须与上面
			// BuildParamLayout 的结果**逐字段相等** —— 名称/类型/偏移/大小/绑定一个都不许放宽。
			{
				const std::string reflectionPath =
					World::MaterialSurfaceCompiler::ReflectionPath(compiled.Artifact);
				CHECK(!reflectionPath.empty());
				CHECK(std::filesystem::is_regular_file(reflectionPath));
				std::ifstream reflectionStream(reflectionPath, std::ios::binary);
				CHECK(reflectionStream.good());
				std::ostringstream reflectionBuffer;
				reflectionBuffer << reflectionStream.rdbuf();
				const std::string reflectionText = reflectionBuffer.str();
				CHECK(!reflectionText.empty());

				World::MaterialParamLayout reflected;
				std::string reflectionError;
				CHECK(World::ReflectParamLayoutFromReflectionJson(reflectionText,
					compiled.Artifact.Bytecode, &reflected, &reflectionError));
				CHECK(reflected.CbufferSet == layout.CbufferSet);
				CHECK(reflected.CbufferBinding == layout.CbufferBinding);
				CHECK(reflected.CbufferSize == layout.CbufferSize);
				CHECK(reflected.Fields.size() == layout.Fields.size());
				for (size_t index = 0; index < reflected.Fields.size(); ++index)
				{
					CHECK(reflected.Fields[index].Name == layout.Fields[index].Name);
					// 反射(SPIR-V 侧)类型逐字节相等;注解类型 (Color/Vec4、Bool/uint) 是
					// **语义覆盖**,用引擎自己的等价判据断言,不看名字猜。
					CHECK(reflected.Fields[index].ReflectedType == layout.Fields[index].ReflectedType);
					CHECK(World::IsReflectedTypeCompatible(layout.Fields[index].Type,
						reflected.Fields[index].ReflectedType));
					CHECK(reflected.Fields[index].Offset == layout.Fields[index].Offset);
					CHECK(reflected.Fields[index].Size == layout.Fields[index].Size);
				}
				CHECK(reflected.Textures.size() == layout.Textures.size());
				for (size_t index = 0; index < reflected.Textures.size(); ++index)
				{
					CHECK(reflected.Textures[index].Name == layout.Textures[index].Name);
					CHECK(reflected.Textures[index].ReflectedType == layout.Textures[index].ReflectedType);
					CHECK(reflected.Textures[index].Set == layout.Textures[index].Set);
					CHECK(reflected.Textures[index].Binding == layout.Textures[index].Binding);
				}
				CHECK(reflected.UsedMembers == layout.UsedMembers);
			}

			// 打包:覆盖优先,缺项用注解默认;偏移直接用反射结果。
			std::vector<MaterialParamOverride> overrides;
			overrides.push_back(MaterialParamOverride { "Roughness", "0.75" });
			std::vector<uint8_t> bytes;
			CHECK(World::PackParamValues(layout, table, overrides, &bytes, &error));
			CHECK(bytes.size() == layout.CbufferSize);
			float packedRoughness = 0.0f;
			std::memcpy(&packedRoughness, bytes.data() + layout.Fields[0].Offset, sizeof(float));
			CHECK(packedRoughness == 0.75f);
			float packedTint[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			std::memcpy(packedTint, bytes.data() + layout.Fields[1].Offset, sizeof(packedTint));
			CHECK(packedTint[0] == 1.0f && packedTint[1] == 0.5f
				&& packedTint[2] == 0.25f && packedTint[3] == 1.0f);
			uint32_t packedGlow = 0u;
			std::memcpy(&packedGlow, bytes.data() + layout.Fields[2].Offset, sizeof(packedGlow));
			CHECK(packedGlow == 1u);

			// ③-1 合规:声明 / 类型 / 绑定一致,而且成员真的被读 → true 且无警告。
			std::vector<std::string> warnings;
			CHECK(World::ValidateParamsWithReflection(annotated, table, &warnings, &error));
			CHECK(warnings.empty());

			// ③-2 声明未用 → 仍是 true,但给可读警告(编辑器照样显示该参数)。
			const std::string unusedSource = "//! param Float Unused = 0.25\n" + annotated;
			std::vector<MaterialParamDecl> unusedTable;
			CHECK(World::ParseMaterialParams(unusedSource, &unusedTable, &error));
			warnings.clear();
			CHECK(World::ValidateParamsWithReflection(unusedSource, unusedTable, &warnings, &error));
			CHECK(warnings.size() == 1);
			CHECK(warnings[0].find("Unused") != std::string::npos);
			CHECK(warnings[0].find("声明未用") != std::string::npos);
			std::printf("World.ShaderPipeline: M4-S2 unused warning: %s\n", warnings[0].c_str());

			// ③-3 用了未声明:没有注解,用户**手写**参数块 → 反射到没声明的成员 → error。
			const std::string handWritten =
				"cbuffer MaterialParams : register(b4, space1)\n"
				"{\n"
				"    float3 Speed;\n"
				"};\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.BaseColor *= Speed;\n"
				"    return surface;\n"
				"}\n";
			std::vector<MaterialParamDecl> noTable;
			warnings.clear();
			CHECK(!World::ValidateParamsWithReflection(handWritten, noTable, &warnings, &error));
			CHECK(error.find("Speed") != std::string::npos);
			CHECK(error.find("没有声明") != std::string::npos);

			// 注解 + 手写块撞同一个寄存器 → 结构化编译错误(不崩、不静默)。
			const std::string conflict = "//! param Float Speed = 1\n" + handWritten;
			std::vector<MaterialParamDecl> conflictTable;
			CHECK(World::ParseMaterialParams(conflict, &conflictTable, &error));
			warnings.clear();
			CHECK(!World::ValidateParamsWithReflection(conflict, conflictTable, &warnings, &error));
			CHECK(!error.empty());

			// M4-S3:贴图参数上限 = 8(t4..t11 的固定槽位)。第 9 张必须结构化失败,
			// 而且**不调用编译器**(表自检在编译之前),不能静默丢参数。
			{
				std::string tooManyTextures;
				for (int index = 0; index < 9; ++index)
					tooManyTextures += "//! param Texture2D Map" + std::to_string(index) + " = \"\"\n";
				tooManyTextures +=
					"Surface Evaluate(MaterialInputs input)\n"
					"{\n"
					"    return MakeDefaultSurface();\n"
					"}\n";
				const size_t toolsBeforeCap = MaterialSurfaceCompiler::ToolInvocationCount();
				const World::SurfaceCompileResult capped =
					MaterialSurfaceCompiler::CompileSurface(tooManyTextures, "m4s3-texture-cap");
				CHECK(!capped.Success);
				CHECK(MaterialSurfaceCompiler::ToolInvocationCount() == toolsBeforeCap);
				bool foundCapDiagnostic = false;
				for (const World::SurfaceDiagnostic& diagnostic : capped.Diagnostics)
				{
					if (diagnostic.Severity == "error"
						&& diagnostic.Message.find("too many Texture2D params") != std::string::npos)
					{
						foundCapDiagnostic = true;
					}
				}
				CHECK(foundCapDiagnostic);
				std::printf("World.ShaderPipeline: M4-S3 texture cap diagnostic: %s\n",
					capped.Diagnostics.empty() ? "<none>" : capped.Diagnostics.front().Message.c_str());
			}

			// 注解坏 → 结构化诊断(用户源行列号),并且不调用编译器。
			const size_t toolsBefore = MaterialSurfaceCompiler::ToolInvocationCount();
			const World::SurfaceCompileResult badAnnotation = MaterialSurfaceCompiler::CompileSurface(
				"//! param Float3 Tint = 1\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    return MakeDefaultSurface();\n"
				"}\n",
				"m4s2-bad-annotation");
			CHECK(!badAnnotation.Success);
			CHECK(MaterialSurfaceCompiler::ToolInvocationCount() == toolsBefore);
			bool foundAnnotationDiagnostic = false;
			for (const World::SurfaceDiagnostic& diagnostic : badAnnotation.Diagnostics)
			{
				if (diagnostic.InUserSource && diagnostic.UserLine == 1 && diagnostic.UserColumn == 11
					&& diagnostic.Message.find("未知参数类型") != std::string::npos)
				{
					foundAnnotationDiagnostic = true;
				}
			}
			CHECK(foundAnnotationDiagnostic);
		}

		// 3c. MAT-FN1b:材质函数(库文件)的 include 搜索路径 + 依赖哈希进缓存键
		{
			using World::MaterialParamDecl;
			using World::MaterialParamLayout;
			using World::MaterialSurfaceCompiler;
			using World::SurfaceCompileResult;
			using World::SurfaceShaderBackend;

			const std::string runTag = temp.path.filename().string();
			// 夹具 = 临时内容根 <temp>/assets/shaders/lib/** 下的**纯函数库**(无入口、无注解、无绑定);
			// 材质侧 `#include "lib/pattern_lib.slang"` 后直接调用。根用**绝对路径**(调用方给,引擎不猜)。
			const std::filesystem::path includeRoot = temp.path / "assets" / "shaders";
			const std::filesystem::path libraryPath = includeRoot / "lib" / "pattern_lib.slang";
			const std::filesystem::path nestedLibraryPath = includeRoot / "lib" / "sub" / "tint_lib.slang";
			const std::string nestedLibrarySource =
				"// 材质函数库(被 pattern_lib.slang 再 include —— 依赖扫描必须递归)\n"
				"// matfn1b nested " + runTag + "\n"
				"float3 MatFnTint(float3 color, float3 tint, float amount)\n"
				"{\n"
				"    return lerp(color, color * tint, saturate(amount));\n"
				"}\n";
			const std::string librarySource =
				"// 材质函数库:纯函数,资源/参数由材质传入(自己不占槽位)\n"
				"// matfn1b library " + runTag + "\n"
				"#include \"sub/tint_lib.slang\"\n"
				"float3 MatFnPattern(float3 color, float2 uv, float scale)\n"
				"{\n"
				"    float checker = step(0.5f, fmod(abs(floor(uv.x * scale) + floor(uv.y * scale)), 2.0f));\n"
				"    return MatFnTint(color, float3(0.9f, 0.6f, 0.3f), checker);\n"
				"}\n";
			WriteText(nestedLibraryPath, nestedLibrarySource);
			WriteText(libraryPath, librarySource);

			const std::string libraryUser = "// 材质侧:include 材质函数库后直接调用\n"
				"#include \"lib/pattern_lib.slang\"\n"
				"// matfn1b user " + runTag + "\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.BaseColor = MatFnPattern(surface.BaseColor, input.UV, 8.0f);\n"
				"    return surface;\n"
				"}\n";

			// ① 搜索根是承重的:不带根 → slangc 找不到库文件(结构化失败);带根 → 编译成功。
			const SurfaceCompileResult withoutRoots = MaterialSurfaceCompiler::CompileSurface(
				libraryUser, "matfn1b-without-roots");
			CHECK(!withoutRoots.Success);
			CHECK(withoutRoots.RawToolOutput.find("pattern_lib.slang") != std::string::npos);

			const SurfaceCompileResult withRoots = MaterialSurfaceCompiler::CompileSurface(
				libraryUser, "matfn1b-with-roots", SurfaceShaderBackend::VulkanSpirV, { includeRoot });
			CHECK(withRoots.Success);
			CHECK(!withRoots.Artifact.Bytecode.empty());
			CHECK(withRoots.Artifact.VertexStages.size() == 3);
			CHECK(withRoots.Artifact.CacheKey.size() == 16);

			// ①b import 与 #include 同规则(Slang 的模块名 → 文件约定 `a.b` → `a/b.slang`,-I 同样生效):
			//     带根编译成功;改被 import 的模块 → 键变化(依赖哈希同样覆盖 import)。
			const std::filesystem::path importedLibraryPath = includeRoot / "lib" / "import_lib.slang";
			const std::string importedLibrarySource =
				"// 材质函数库(被 import 引用;模块文件导出的符号要 public)\n"
				"// matfn1b import " + runTag + "\n"
				"public float3 MatFnImportBlend(float3 color, float3 tint)\n"
				"{\n"
				"    return lerp(color, tint, 0.25f);\n"
				"}\n";
			WriteText(importedLibraryPath, importedLibrarySource);
			const std::string importUser = "// 材质侧:import 材质函数库(与 #include 同一套搜索规则)\n"
				"import lib.import_lib;\n"
				"// matfn1b import user " + runTag + "\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.BaseColor = MatFnImportBlend(surface.BaseColor, float3(0.2f, 0.4f, 0.6f));\n"
				"    return surface;\n"
				"}\n";
			const SurfaceCompileResult importFirst = MaterialSurfaceCompiler::CompileSurface(
				importUser, "matfn1b-import", SurfaceShaderBackend::VulkanSpirV, { includeRoot });
			CHECK(importFirst.Success);
			CHECK(importFirst.Artifact.VertexStages.size() == 3);
			WriteText(importedLibraryPath, importedLibrarySource + "// 改库:import 的模块内容也进键\n");
			const SurfaceCompileResult importSecond = MaterialSurfaceCompiler::CompileSurface(
				importUser, "matfn1b-import", SurfaceShaderBackend::VulkanSpirV, { includeRoot });
			CHECK(importSecond.Success);
			CHECK(!importSecond.CacheHit);
			CHECK(importSecond.Artifact.CacheKey != importFirst.Artifact.CacheKey);
			std::printf("World.ShaderPipeline: MAT-FN1b import keys before=%s after=%s\n",
				importFirst.Artifact.CacheKey.c_str(), importSecond.Artifact.CacheKey.c_str());

			// ② 改库(材质源一字不动)→ 依赖哈希进键 → 键变化 + 真的重编译。
			std::string editedNested = nestedLibrarySource;
			const size_t amountTextAt = editedNested.find("saturate(amount)");
			CHECK(amountTextAt != std::string::npos);
			editedNested.replace(amountTextAt, std::string("saturate(amount)").size(),
				"saturate(amount * 1.25f)");
			CHECK(editedNested != nestedLibrarySource);
			WriteText(nestedLibraryPath, editedNested);
			const size_t missesBeforeLibraryEdit = MaterialSurfaceCompiler::CacheMissCount();
			const SurfaceCompileResult withEditedLibrary = MaterialSurfaceCompiler::CompileSurface(
				libraryUser, "matfn1b-with-roots", SurfaceShaderBackend::VulkanSpirV, { includeRoot });
			CHECK(withEditedLibrary.Success);
			CHECK(!withEditedLibrary.CacheHit);
			CHECK(withEditedLibrary.Artifact.CacheKey != withRoots.Artifact.CacheKey);
			CHECK(MaterialSurfaceCompiler::CacheMissCount() == missesBeforeLibraryEdit + 1);
			// 库里改的是真代码(不是注释)→ 产物字节也必须跟着变,不是"换了键还是旧产物"。
			CHECK(withEditedLibrary.Artifact.Bytecode != withRoots.Artifact.Bytecode);
			std::printf("World.ShaderPipeline: MAT-FN1b library keys base=%s edited=%s\n",
				withRoots.Artifact.CacheKey.c_str(), withEditedLibrary.Artifact.CacheKey.c_str());

			// ③ 嵌套库被删 → 结构化诊断带文件名(依赖扫描不短路,让 slangc 报错;不崩)。
			std::error_code removeLibraryEc;
			CHECK(std::filesystem::remove(nestedLibraryPath, removeLibraryEc));
			const SurfaceCompileResult missingLibrary = MaterialSurfaceCompiler::CompileSurface(
				libraryUser, "matfn1b-missing-library", SurfaceShaderBackend::VulkanSpirV, { includeRoot });
			CHECK(!missingLibrary.Success);
			bool mentionsMissingFile = missingLibrary.RawToolOutput.find("tint_lib.slang") != std::string::npos;
			bool hasErrorDiagnostic = false;
			for (const World::SurfaceDiagnostic& diagnostic : missingLibrary.Diagnostics)
			{
				if (diagnostic.Severity == "error")
					hasErrorDiagnostic = true;
				if (diagnostic.Message.find("tint_lib.slang") != std::string::npos)
					mentionsMissingFile = true;
			}
			CHECK(mentionsMissingFile);
			CHECK(hasErrorDiagnostic);
			std::printf("World.ShaderPipeline: MAT-FN1b missing library raw=%.200s\n",
				missingLibrary.RawToolOutput.c_str());
			// 恢复夹具:后面的检查继续用这份库(③ 的失败键目录留着,不清理)。
			WriteText(nestedLibraryPath, editedNested);

			// ④ 源里没有 include/import → 传不传根,键与产物字节都必须一致(默认空 ⇒ 逐字节不变)。
			const std::string noIncludeSource = "Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.Roughness = saturate(0.25f + input.UV.x * 0.0f);\n"
				"    return surface;\n"
				"}\n"
				"// matfn1b no-include baseline " + runTag + "\n";
			const size_t missesBeforeNoInclude = MaterialSurfaceCompiler::CacheMissCount();
			const SurfaceCompileResult noIncludePlain = MaterialSurfaceCompiler::CompileSurface(
				noIncludeSource, "matfn1b-no-include");
			CHECK(noIncludePlain.Success);
			CHECK(MaterialSurfaceCompiler::CacheMissCount() == missesBeforeNoInclude + 1);
			const SurfaceCompileResult noIncludeWithRoots = MaterialSurfaceCompiler::CompileSurface(
				noIncludeSource, "matfn1b-no-include", SurfaceShaderBackend::VulkanSpirV,
				{ includeRoot, temp.path });
			CHECK(noIncludeWithRoots.Success);
			CHECK(noIncludeWithRoots.CacheHit);   // 同一个键 → 命中,没有多编译一次
			CHECK(noIncludeWithRoots.Artifact.CacheKey == noIncludePlain.Artifact.CacheKey);
			CHECK(noIncludeWithRoots.Artifact.Bytecode == noIncludePlain.Artifact.Bytecode);
			std::printf("World.ShaderPipeline: MAT-FN1b no-include key=%s (roots 不影响)\n",
				noIncludePlain.Artifact.CacheKey.c_str());

			// ⑤ 真实示例(不是手工 slangc):ShowcaseMaterial.slang 的 `#include "lib/pattern.slang"`
			// 走**引擎路径**在两个后端都编过。
			const std::filesystem::path shadersRoot =
				std::filesystem::path(WLD_ASSETPATH) / "shaders";
			const std::filesystem::path showcasePath = shadersRoot / "examples"
				/ "ShowcaseMaterial.slang";
			CHECK(std::filesystem::is_regular_file(showcasePath));
			std::string showcaseSource;
			{
				std::ifstream stream(showcasePath, std::ios::binary);
				CHECK(stream.good());
				std::ostringstream buffer;
				buffer << stream.rdbuf();
				showcaseSource = buffer.str();
			}
			std::vector<MaterialParamDecl> showcaseTable;
			std::string showcaseError;
			CHECK(World::ParseMaterialParams(showcaseSource, &showcaseTable, &showcaseError));
			const SurfaceCompileResult showcaseVulkan = MaterialSurfaceCompiler::CompileSurfaceWithParams(
				showcaseSource, showcaseTable, "shaders/examples/ShowcaseMaterial.slang",
				SurfaceShaderBackend::VulkanSpirV, { shadersRoot });
			CHECK(showcaseVulkan.Success);
			CHECK(showcaseVulkan.Artifact.Backend == "vulkan-spirv");
			CHECK(showcaseVulkan.Artifact.VertexStages.size() == 3);
			const SurfaceCompileResult showcaseOpenGl = MaterialSurfaceCompiler::CompileSurfaceWithParams(
				showcaseSource, showcaseTable, "shaders/examples/ShowcaseMaterial.slang",
				SurfaceShaderBackend::OpenGLSpirV, { shadersRoot });
			CHECK(showcaseOpenGl.Success);
			CHECK(showcaseOpenGl.Artifact.Backend == "opengl-spirv");
			CHECK(showcaseOpenGl.Artifact.VertexStages.size() == 3);
			CHECK(showcaseOpenGl.Artifact.Bytecode != showcaseVulkan.Artifact.Bytecode);
			uint32_t showcaseGlVersion = 0;
			std::memcpy(&showcaseGlVersion, showcaseOpenGl.Artifact.Bytecode.data() + 4,
				sizeof(showcaseGlVersion));
			CHECK(showcaseGlVersion == 0x00010000u);   // GL 目标:SPIR-V 1.0(ARB_gl_spirv)
			std::printf("World.ShaderPipeline: MAT-FN1b showcase keys vulkan=%s gl=%s\n",
				showcaseVulkan.Artifact.CacheKey.c_str(), showcaseOpenGl.Artifact.CacheKey.c_str());

			// ⑥ BuildParamLayout 同样接受 include 根并向下透传(同一个源:带根能反射,不带根失败)。
			const std::string libraryUserWithParam = "//! param Float PatternScale = 8 [1, 64]\n"
				"// 材质侧:include 材质函数库后直接调用\n"
				"#include \"lib/pattern_lib.slang\"\n"
				"// matfn1b user param " + runTag + "\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.BaseColor = MatFnPattern(surface.BaseColor, input.UV, PatternScale);\n"
				"    return surface;\n"
				"}\n";
			std::vector<MaterialParamDecl> libraryUserTable;
			std::string libraryTableError;
			CHECK(World::ParseMaterialParams(libraryUserWithParam, &libraryUserTable, &libraryTableError));
			CHECK(libraryUserTable.size() == 1);
			MaterialParamLayout libraryLayout;
			std::string layoutError;
			CHECK(World::BuildParamLayout(libraryUserWithParam, libraryUserTable,
				&libraryLayout, &layoutError, { includeRoot }));
			CHECK(libraryLayout.Fields.size() == 1);
			CHECK(libraryLayout.Fields[0].Name == "PatternScale");
			CHECK(libraryLayout.CbufferSize >= 16);
			CHECK(!World::BuildParamLayout(libraryUserWithParam, libraryUserTable,
				&libraryLayout, &layoutError, std::vector<std::filesystem::path>()));
			CHECK(!layoutError.empty());
			std::printf("World.ShaderPipeline: MAT-FN1b BuildParamLayout with roots ok "
				"(fields=%zu size=%u)\n", libraryLayout.Fields.size(), libraryLayout.CbufferSize);

			// ⑦ 真实库文件的重烘演示(**不碰仓库资产**):把 shaders/lib 复制到临时目录,改副本里
			//    pattern.slang 一个常量 → 同一个真实材质的键变化 + 真重编译 + 产物字节跟着变。
			const std::filesystem::path copiedShaders = temp.path / "shaders-snapshot";
			const std::filesystem::path realLibraryPath = shadersRoot / "lib" / "pattern.slang";
			CHECK(std::filesystem::is_regular_file(realLibraryPath));
			std::string copiedLibrarySource;
			{
				std::ifstream stream(realLibraryPath, std::ios::binary);
				CHECK(stream.good());
				std::ostringstream buffer;
				buffer << stream.rdbuf();
				copiedLibrarySource = buffer.str();
			}
			CHECK(!copiedLibrarySource.empty());
			const std::filesystem::path copiedLibrary = copiedShaders / "lib" / "pattern.slang";
			WriteText(copiedLibrary, copiedLibrarySource);   // 建父目录并落盘副本
			CHECK(std::filesystem::is_regular_file(copiedLibrary));
			const SurfaceCompileResult realLibraryBefore = MaterialSurfaceCompiler::CompileSurfaceWithParams(
				showcaseSource, showcaseTable, "matfn1b-real-library",
				SurfaceShaderBackend::VulkanSpirV, { copiedShaders });
			CHECK(realLibraryBefore.Success);
			const size_t parityStepAt = copiedLibrarySource.find("step(0.5, parity)");
			CHECK(parityStepAt != std::string::npos);
			copiedLibrarySource.replace(parityStepAt, std::string("step(0.5, parity)").size(),
				"step(0.55, parity)");
			WriteText(copiedLibrary, copiedLibrarySource);
			const SurfaceCompileResult realLibraryAfter = MaterialSurfaceCompiler::CompileSurfaceWithParams(
				showcaseSource, showcaseTable, "matfn1b-real-library",
				SurfaceShaderBackend::VulkanSpirV, { copiedShaders });
			CHECK(realLibraryAfter.Success);
			CHECK(!realLibraryAfter.CacheHit);
			CHECK(realLibraryAfter.Artifact.CacheKey != realLibraryBefore.Artifact.CacheKey);
			CHECK(realLibraryAfter.Artifact.Bytecode != realLibraryBefore.Artifact.Bytecode);
			std::printf("World.ShaderPipeline: MAT-FN1b real lib rebake before=%s after=%s\n",
				realLibraryBefore.Artifact.CacheKey.c_str(), realLibraryAfter.Artifact.CacheKey.c_str());
		}

		std::printf("World.ShaderPipeline: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.ShaderPipeline: FAILED: %s\n", error.what());
		return 1;
	}
}
