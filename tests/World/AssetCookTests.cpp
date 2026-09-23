#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Asset/ScriptArtifact.h"
#include "World/Core/Log.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	using namespace World::Asset;

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
			path = std::filesystem::temp_directory_path(ec) / ("we-asset-" + std::to_string(unique));
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

	void WriteBytes(const std::filesystem::path& path, const std::string& content)
	{
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		if (ec)
			throw std::runtime_error("create_directories failed: " + path.string());
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
		if (!out)
			throw std::runtime_error("failed to write " + path.string());
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		return text;
	}

	std::vector<uint8_t> ReadBytes(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			throw std::runtime_error("failed to open " + path.string());
		std::vector<uint8_t> bytes;
		char buffer[4096];
		while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0)
		{
			const size_t count = static_cast<size_t>(in.gcount());
			for (size_t index = 0; index < count; ++index)
				bytes.push_back(static_cast<uint8_t>(buffer[index]));
		}
		return bytes;
	}

	bool Contains(const std::string& haystack, const std::string& needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	std::shared_ptr<IAssetImporter> FindImporter(
		const std::vector<std::shared_ptr<IAssetImporter>>& importers, const std::string& name)
	{
		for (const std::shared_ptr<IAssetImporter>& candidate : importers)
			if (candidate && candidate->Name() == name)
				return candidate;
		return nullptr;
	}

	bool StartsWithWsl1(const std::vector<uint8_t>& bytes)
	{
		return bytes.size() > 28 && std::memcmp(bytes.data(), "WSL1", 4) == 0;
	}
}

int main()
{
	World::Log::Init();
	try
	{
		TempDir temp;

		// 1. 清单往返与非法值拒绝
		{
			std::string error;
			ProjectManifest manifest;
			manifest.Id = "com.test.game";
			manifest.ContentRoot = "content";
			manifest.StartScene = "scenes/a.wd";
			manifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path manifestPath = temp.path / "project.we.yaml";
			CHECK(ProjectManifest::Save(manifestPath, manifest, &error));
			CHECK(error.empty());

			ProjectManifest loaded;
			CHECK(ProjectManifest::Load(manifestPath, &loaded, &error));
			CHECK(loaded.Id == "com.test.game");
			CHECK(loaded.ContentRoot == "content");
			CHECK(loaded.StartScene == "scenes/a.wd");
			CHECK(loaded.Packages.size() == 1 && loaded.Packages[0] == "packages/Base.wpak");
			CHECK(loaded.ResolveContentRoot(manifestPath) ==
				(manifestPath.parent_path() / "content").lexically_normal());

			ProjectManifest bad = manifest;
			bad.Id.clear();
			CHECK(!ProjectManifest::Save(temp.path / "bad.yaml", bad, &error));
			CHECK(!error.empty());
			bad = manifest;
			bad.StartScene = "../evil";
			CHECK(!ProjectManifest::Save(temp.path / "bad2.yaml", bad, &error));
			CHECK(!error.empty());

			std::filesystem::path located;
			CHECK(ProjectManifest::Locate(temp.path, &located));
			CHECK(located == manifestPath);
		}

		// 2. 增量烘焙:未变跳过、变化重处理、数据库跨实例持久化
		{
			const std::filesystem::path content = temp.path / "content";
			WriteBytes(content / "scenes" / "a.wd", "scene-v1");
			WriteBytes(content / "textures" / "x.png", "tex-bytes");

			std::string error;
			ProjectManifest manifest;
			manifest.Id = "com.test.game";
			manifest.ContentRoot = "content";
			manifest.StartScene = "scenes/a.wd";
			manifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path manifestPath = temp.path / "project.we.yaml";
			CHECK(ProjectManifest::Save(manifestPath, manifest, &error));
			const std::filesystem::path outputDir = temp.path / "cooked-output";

			CookPipeline pipeline(DefaultImporters());
			CookSummary summary;
			std::vector<CookEntryResult> results =
				pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 2 && summary.Changed == 2 && summary.Failed == 0);
			CHECK(ReadText(outputDir / "cooked" / "scenes" / "a.wd") == "scene-v1");
			CHECK(std::filesystem::is_regular_file(outputDir / "cook.db.json"));

			results = pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 2 && summary.Skipped == 2 && summary.Changed == 0);

			WriteBytes(content / "scenes" / "a.wd", "scene-v2");
			results = pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 1 && summary.Skipped == 1);
			CHECK(ReadText(outputDir / "cooked" / "scenes" / "a.wd") == "scene-v2");

			// 新实例从数据库恢复:全部跳过。
			CookPipeline second(DefaultImporters());
			results = second.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 0 && summary.Skipped == 2);

			// 3. pipeline 产物打包 → PackageProvider 读回一致
			const std::filesystem::path pakPath = temp.path / "out.wpak";
			std::error_code pakEc;
			CHECK(World::Vfs::PackageProvider::BuildFromDirectory(outputDir / "cooked", pakPath, pakEc));
			std::shared_ptr<World::Vfs::PackageProvider> provider =
				World::Vfs::PackageProvider::Open(pakPath, pakEc);
			CHECK(provider != nullptr);
			std::vector<uint8_t> bytes;
			CHECK(provider->Open("scenes/a.wd", bytes, pakEc));
			CHECK(std::string(bytes.begin(), bytes.end()) == "scene-v2");
		}

		// 4. W7-2:Script 导入器 v2(.lua + .luau)→ WSL1 容器;可 Unpack / LoadChunk;增量稳定。
		{
			const std::vector<std::shared_ptr<IAssetImporter>> importers = DefaultImporters();
			// D5b-1:Model 导入器(.gltf/.glb)注册在 Script 之后、PassThrough 之前。
			// M4-S2:MaterialShader 导入器(.hlsl)注册在 Model 之后、PassThrough 之前。
			CHECK(importers.size() == 5);
			CHECK(importers[0]->Name() == "Scene" && importers[0]->Version() == 1);
			CHECK(importers[2]->Name() == "Model" && importers[2]->Version() == 1);
			CHECK(importers[3]->Name() == "MaterialShader" && importers[3]->Version() == 1);
			CHECK(importers[4]->Name() == "PassThrough");
			const std::shared_ptr<IAssetImporter> script = FindImporter(importers, "Script");
			CHECK(script != nullptr);
			CHECK(script->Version() == 2);
			CHECK(script->Matches("scripts/Test.lua"));
			CHECK(script->Matches("scripts/Typed.luau"));
			CHECK(!script->Matches("scenes/a.wd"));
			CHECK(!script->Matches("textures/x.png"));
			CHECK(!importers[0]->Matches("scripts/Typed.luau"));   // Scene 不吞脚本扩展名

			const std::filesystem::path content = temp.path / "script-content";
			const std::string luaSource = "local W7 = 41\nreturn W7 + 1\n";
			const std::string luauSource = "local n: number = 2\nreturn n * 3\n";
			WriteBytes(content / "scripts" / "Test.lua", luaSource);
			WriteBytes(content / "scripts" / "Typed.luau", luauSource);

			std::string error;
			ProjectManifest manifest;
			manifest.Id = "com.test.scripts";
			manifest.ContentRoot = "script-content";
			manifest.StartScene = "scripts/Test.lua";
			manifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path manifestPath = temp.path / "scripts.we.yaml";
			CHECK(ProjectManifest::Save(manifestPath, manifest, &error));
			const std::filesystem::path outputDir = temp.path / "script-cooked";

			CookPipeline pipeline(DefaultImporters());
			CookSummary summary;
			std::vector<CookEntryResult> results =
				pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 2 && summary.Changed == 2 && summary.Skipped == 0 && summary.Failed == 0);
			CHECK(results.size() == 2 && !results[0].Failed && !results[1].Failed);

			const std::filesystem::path luaArtifact = outputDir / "cooked" / "scripts" / "Test.lua";
			const std::filesystem::path luauArtifact = outputDir / "cooked" / "scripts" / "Typed.luau";
			const std::vector<uint8_t> luaBytes = ReadBytes(luaArtifact);
			const std::vector<uint8_t> luauBytes = ReadBytes(luauArtifact);
			CHECK(StartsWithWsl1(luaBytes));
			CHECK(StartsWithWsl1(luauBytes));
			CHECK(luaBytes.size() != luaSource.size());   // 包内不再是源码副本
			CHECK(luauBytes.size() != luauSource.size());
			std::printf("[W7-2] (4) containers: Test.lua %zu bytes (source %zu), Typed.luau %zu bytes (source %zu)\n",
				luaBytes.size(), luaSource.size(), luauBytes.size(), luauSource.size());

			// Unpack 取出 payload,并经统一入口 LoadChunk 实际装载执行。
			std::vector<uint8_t> payload;
			CHECK(ScriptArtifact::Unpack("scripts/Test.lua", luaBytes.data(), luaBytes.size(), payload, &error));
			CHECK(!payload.empty());
			CHECK(payload.size() == luaBytes.size() - 28);
			World::LuauVm vm;
			CHECK(vm.Init(&error));
			const World::ScriptTableRef environment = vm.CreateEnvironment();
			CHECK(environment.IsValid());
			World::ScriptFunctionRef function =
				vm.LoadChunk(luaBytes, "scripts/Test.lua", environment, &error);
			CHECK(function.IsValid());
			World::ScriptValue returned;
			CHECK(function.Call(nullptr, 0, &returned, &error));
			double value = 0.0;
			CHECK(returned.AsNumber(&value));
			CHECK(value == 42.0);
			vm.Shutdown();

			// 增量:未变化 → skip(指纹稳定);改源码 → 只有它 changed。
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 0 && summary.Skipped == 2 && summary.Failed == 0);
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 0 && summary.Skipped == 2);
			WriteBytes(content / "scripts" / "Test.lua", "return 7\n");
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 1 && summary.Skipped == 1 && summary.Failed == 0);
			const std::vector<uint8_t> changedBytes = ReadBytes(luaArtifact);
			CHECK(StartsWithWsl1(changedBytes));
			CHECK(changedBytes != luaBytes);
			CHECK(ReadBytes(luauArtifact) == luauBytes);
			std::printf("[W7-2] (4) incremental: 2 changed -> 2 skipped -> 1 changed/1 skipped\n");
		}

		// 5. W7-2:坏脚本 → Import 失败(ec = invalid_argument、Data 空、诊断带路径与 compile error),
		//    同一条门禁在 cook 上表现为 Failed 且不落产物。
		{
			const std::filesystem::path badSource = temp.path / "bad-content" / "scripts" / "Broken.lua";
			WriteBytes(badSource, "local x = \n");

			const std::shared_ptr<IAssetImporter> script = FindImporter(DefaultImporters(), "Script");
			CHECK(script != nullptr);
			ImportRequest request;
			request.LogicalPath = "scripts/Broken.lua";
			request.Source = badSource;
			std::error_code importEc;
			const ImportResult imported = script->Import(request, importEc);
			CHECK(!imported.Ok);
			CHECK(imported.Data.empty());
			CHECK(imported.Fingerprint == 0);   // 导入器不写 Fingerprint(复合指纹归 CookPipeline)
			CHECK(Contains(imported.Error, "compile error"));
			CHECK(Contains(imported.Error, "scripts/Broken.lua"));
			CHECK(importEc == std::make_error_code(std::errc::invalid_argument));
			std::printf("[W7-2] (5) bad script: ec=%s | error=%s\n",
				importEc.message().c_str(), imported.Error.c_str());

			std::string error;
			ProjectManifest manifest;
			manifest.Id = "com.test.badscripts";
			manifest.ContentRoot = "bad-content";
			manifest.StartScene = "scripts/Broken.lua";
			manifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path manifestPath = temp.path / "bad-scripts.we.yaml";
			CHECK(ProjectManifest::Save(manifestPath, manifest, &error));
			const std::filesystem::path outputDir = temp.path / "bad-script-cooked";

			CookPipeline pipeline(DefaultImporters());
			CookSummary summary;
			const std::vector<CookEntryResult> results =
				pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 1 && summary.Failed == 1 && summary.Changed == 0);
			CHECK(results.size() == 1 && results[0].Failed);
			CHECK(Contains(results[0].Error, "compile error"));
			CHECK(!std::filesystem::exists(outputDir / "cooked" / "scripts" / "Broken.lua"));
			std::printf("[W7-2] (5) cook gate: 1/1 failed, no artifact written\n");
		}

		// 5b. M4-S2:`.hlsl`(MaterialShader)是一等资产 —— 原样复制;cook 指纹 = 源内容哈希,
		//     改代码/注解自动重烘,没改则跳过。
		{
			const std::shared_ptr<IAssetImporter> shader = FindImporter(DefaultImporters(), "MaterialShader");
			CHECK(shader != nullptr);
			CHECK(shader->Version() == 1);
			CHECK(shader->Matches("shaders/Glow.hlsl"));
			CHECK(!shader->Matches("materials/glass.wmat"));
			CHECK(!shader->Matches("scenes/a.wd"));
			CHECK(!shader->Matches("textures/x.png"));

			const std::filesystem::path content = temp.path / "shader-content";
			const std::filesystem::path source = content / "shaders" / "Glow.hlsl";
			const std::string firstSource =
				"//! param Float Roughness = 0.25 [0,1]\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.Roughness = Roughness;\n"
				"    return surface;\n"
				"}\n";
			WriteBytes(source, firstSource);

			std::string error;
			ProjectManifest manifest;
			manifest.Id = "com.test.shaders";
			manifest.ContentRoot = "shader-content";
			manifest.StartScene = "shaders/Glow.hlsl";
			manifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path manifestPath = temp.path / "shaders.we.yaml";
			CHECK(ProjectManifest::Save(manifestPath, manifest, &error));
			const std::filesystem::path outputDir = temp.path / "shader-cooked";

			CookPipeline pipeline(DefaultImporters());
			CookSummary summary;
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 1 && summary.Changed == 1 && summary.Failed == 0);
			const std::filesystem::path artifact = outputDir / "cooked" / "shaders" / "Glow.hlsl";
			CHECK(ReadText(artifact) == firstSource);   // 原样复制(注解也在产物里)

			// 未改 → skip;改注解/代码 → 重烘(指纹 = 源内容哈希)。
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 0 && summary.Skipped == 1 && summary.Failed == 0);
			const std::string secondSource =
				"//! param Float Roughness = 0.5 [0,1]\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.Roughness = Roughness;\n"
				"    return surface;\n"
				"}\n";
			WriteBytes(source, secondSource);
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 1 && summary.Skipped == 0 && summary.Failed == 0);
			CHECK(ReadText(artifact) == secondSource);
			std::printf("[M4-S2] (5b) .hlsl cook: 1 changed -> 1 skipped -> 1 changed (content hash)\n");
		}

		std::printf("World.Asset: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Asset: FAILED: %s\n", error.what());
		return 1;
	}
}
