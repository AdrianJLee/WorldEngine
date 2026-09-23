#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/MaterialSurface.h"
#include "World/Renderer/ShaderUtils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

			// ①c M4-S3(D5):顶点阶段齐全(模板自带的三个入口)+ **按模板键缓存** ——
			// 改用户代码时只有 PSMain 真的跑 dxc(稳态下每次编辑一次),VS 全部命中缓存。
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
				std::printf("World.ShaderPipeline: M4-S3 vertex stages=%zu (dxc runs: first=%zu, edited=%zu)\n",
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

		// 3b. M4-S2:注解参数表 → 参数块编译 → dxc 反射校验(三态)+ 字段偏移表 + 打包
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
				"    surface.BaseColor = Tint.rgb * Albedo.Sample(AlbedoSampler, input.UV).rgb;\n"
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
			// M4-S3(D1):参数块从 space1 b2 挪到 b4(GL 的 UBO 单元 = binding,单元 2 是灯光)。
			CHECK(wrapper.find("cbuffer MaterialParams : register(b4, space1)") != std::string::npos);
			CHECK(wrapper.find("float Roughness;") != std::string::npos);
			CHECK(wrapper.find("float4 Tint;") != std::string::npos);
			CHECK(wrapper.find("bool Glow;") != std::string::npos);
			CHECK(wrapper.find("Texture2D Albedo : register(t4, space2);") != std::string::npos);
			CHECK(wrapper.find("SamplerState AlbedoSampler : register(s4, space2);") != std::string::npos);

			// 反射布局:名称/类型/偏移/大小来自 dxc 的 SPIR-V 汇编(引擎不手写参数结构体)。
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
			// 而且**不调用 dxc**(表自检在编译之前),不能静默丢参数。
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

			// 注解坏 → 结构化诊断(用户源行列号),并且不调用 dxc。
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

		std::printf("World.ShaderPipeline: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.ShaderPipeline: FAILED: %s\n", error.what());
		return 1;
	}
}
