#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/MaterialSurface.h"
#include "World/Renderer/ShaderUtils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

	bool HasVendorTools()
	{
		std::error_code ec;
		const std::filesystem::path dxcDir(WLD_DXC_DIR);
		return std::filesystem::is_regular_file(dxcDir / "dxc.exe", ec) &&
			std::filesystem::is_regular_file(dxcDir / "dxcompiler.dll", ec) &&
			std::filesystem::is_regular_file(
				std::filesystem::path(WLD_ROOT_DIR) / "vendor/SPIRV-Cross/spirv-cross.exe", ec);
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
			CHECK(World::ShaderCompiler::ArtifactLogicalPath("assets/shaders/Foo.hlsl", "VSMain", true) ==
				"shaders/Foo.VSMain.spv");
			CHECK(World::ShaderCompiler::ArtifactLogicalPath("assets/shaders/Foo.hlsl", "PSMain", false) ==
				"shaders/Foo.PSMain.glsl");
		}

		// 2. 解析器优先:有烘焙产物时不触发工具调用(发行形态路径)
		{
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
				World::ShaderCompiler::CompileOrLoad("assets/shaders/Missing.hlsl", "VSMain", "vs_6_0");
			CHECK(bytes.size() == 4);
			CHECK(static_cast<unsigned char>(bytes[0]) == 0xDE);
			CHECK(World::ShaderCompiler::CookedHitCount() == 1);
			CHECK(World::ShaderCompiler::ToolInvocationCount() == 0);
			World::ShaderCompiler::ClearArtifactResolver();
		}

		if (!HasVendorTools())
		{
			std::printf("World.ShaderPipeline: vendor dxc/spirv-cross missing, bake checks skipped\n");
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

			// ①b 删掉缓存文件强制走一次真实 dxc,再验证工具输出逐字节确定。
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

			// 第一版只支持 Vulkan:OpenGL 目标返回结构化提示,不调用工具。
			const size_t toolRunsBeforeOpenGL = MaterialSurfaceCompiler::ToolInvocationCount();
			const World::SurfaceCompileResult openGlUnsupported =
				MaterialSurfaceCompiler::CompileSurface(minimalSource, "m4s1-opengl",
					SurfaceShaderBackend::OpenGLGlsl);
			CHECK(!openGlUnsupported.Success);
			CHECK(!openGlUnsupported.Diagnostics.empty());
			CHECK(openGlUnsupported.Diagnostics[0].Message.find("not supported") != std::string::npos);
			CHECK(MaterialSurfaceCompiler::ToolInvocationCount() == toolRunsBeforeOpenGL);

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

		// 4. 烘焙 → 内容寻址缓存 → 打包 → 包内读回 → 运行时解析器命中
		// 每次运行内容不同 → 指纹不同,保证真的走一次 dxc + spirv-cross;
		// 第二次调用同一内容则必须命中缓存。
		WriteText(sourceDir / "Probe.hlsl",
			std::string(kProbeShader) + "\n// build " + temp.path.filename().string() + "\n");

		World::ShaderCompiler::ResetCounters();
		const World::ShaderCompiler::BakeResult baked =
			World::ShaderCompiler::BakeDirectory(sourceDir, cookedDir);
		CHECK(baked.Shaders == 1);
		CHECK(baked.Artifacts == 4);
		CHECK(baked.Failed == 0);
		const size_t toolRuns = World::ShaderCompiler::ToolInvocationCount();
		CHECK(toolRuns >= 2);

		const std::filesystem::path shaderOut = cookedDir / "shaders";
		for (const char* name : { "Probe.VSMain.spv", "Probe.VSMain.glsl", "Probe.PSMain.spv", "Probe.PSMain.glsl" })
		{
			std::error_code sizeEc;
			CHECK(std::filesystem::is_regular_file(shaderOut / name, sizeEc));
			CHECK(std::filesystem::file_size(shaderOut / name, sizeEc) > 0);
		}

		// 同一内容再次烘焙:命中缓存,工具调用次数不增长。
		const World::ShaderCompiler::BakeResult again =
			World::ShaderCompiler::BakeDirectory(sourceDir, cookedDir);
		CHECK(again.Artifacts == 4 && again.Failed == 0);
		CHECK(World::ShaderCompiler::ToolInvocationCount() == toolRuns);
		CHECK(World::ShaderCompiler::CacheHitCount() >= 2);

		// 打包为 wpak(与 Editor cook 的产物目录一致)并从包内读回。
		const std::filesystem::path pakPath = temp.path / "content.wpak";
		std::error_code pakEc;
		CHECK(World::Vfs::PackageProvider::BuildFromDirectory(cookedDir, pakPath, pakEc));
		std::shared_ptr<World::Vfs::PackageProvider> provider =
			World::Vfs::PackageProvider::Open(pakPath, pakEc);
		CHECK(provider != nullptr);

		const bool vulkan = World::Renderer::GetAPI() == World::RendererAPI::API::Vulkan;
		const std::string expectedName = std::string("shaders/Probe.VSMain.") + (vulkan ? "spv" : "glsl");
		std::vector<uint8_t> expected;
		CHECK(provider->Open(expectedName, expected, pakEc));
		CHECK(!expected.empty());

		// Runtime 侧:注册 VFS 解析器后,着色器不再走源码树编译。
		World::ShaderCompiler::ResetCounters();
		World::ShaderCompiler::SetArtifactResolver(
			[&provider](const std::string& logical, std::vector<uint8_t>& out)
			{
				std::error_code readEc;
				return provider->Open(logical, out, readEc) && !out.empty();
			});
		const std::vector<char> packaged =
			World::ShaderCompiler::CompileOrLoad("assets/shaders/Probe.hlsl", "VSMain", "vs_6_0");
		CHECK(packaged.size() == expected.size());
		CHECK(World::ShaderCompiler::CookedHitCount() == 1);
		CHECK(World::ShaderCompiler::ToolInvocationCount() == 0);
		World::ShaderCompiler::ClearArtifactResolver();

		std::printf("World.ShaderPipeline: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.ShaderPipeline: FAILED: %s\n", error.what());
		return 1;
	}
}
