#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Renderer/Renderer.h"
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

		// 3. 烘焙 → 内容寻址缓存 → 打包 → 包内读回 → 运行时解析器命中
		TempDir temp;
		const std::filesystem::path sourceDir = temp.path / "assets" / "shaders";
		const std::filesystem::path cookedDir = temp.path / "cooked";
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
