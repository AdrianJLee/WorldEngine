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
#include <iterator>
#include <memory>
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

	// Slang-B1w:v1→v2 重烘 A/B 用的最小导入器(原样复制;版本号可控)。
	// 机制与 MaterialShader 导入器同类:复合指纹 = 内容 ⊕ 名字 ⊕ 版本 ⊕ 设置,
	// 所以"只有版本变了"也必须重烘。
	class VersionedCopyImporter final : public IAssetImporter
	{
	public:
		explicit VersionedCopyImporter(uint32_t version) : m_Version(version) {}

		std::string Name() const override { return "TestVersionedCopy"; }
		uint32_t Version() const override { return m_Version; }
		bool Matches(const std::filesystem::path& source) const override
		{
			return source.extension() == ".vsrc";
		}
		ImportResult Import(const ImportRequest& request, std::error_code& ec) const override
		{
			ImportResult result;
			std::ifstream stream(request.Source, std::ios::binary);
			if (!stream)
			{
				ec = std::make_error_code(std::errc::no_such_file_or_directory);
				result.Error = "cannot open " + request.Source.string();
				return result;
			}
			const std::string text((std::istreambuf_iterator<char>(stream)),
				std::istreambuf_iterator<char>());
			result.Data.assign(text.begin(), text.end());
			result.Ok = true;
			return result;
		}

	private:
		uint32_t m_Version = 1;
	};
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
			// M4-S2/Slang-B1:MaterialShader 导入器(规范扩展名 `.slang`;legacy `.hlsl` 同列)
			// 注册在 Model 之后、PassThrough 之前。
			CHECK(importers.size() == 5);
			CHECK(importers[0]->Name() == "Scene" && importers[0]->Version() == 1);
			CHECK(importers[2]->Name() == "Model" && importers[2]->Version() == 1);
			// Slang-B1:升 v2 —— 扩展名口径变化,cook 复合指纹含导入器版本 → 旧 cook.db 整体重烘。
			CHECK(importers[3]->Name() == "MaterialShader" && importers[3]->Version() == 2);
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

		// 5b. M4-S2/Slang-B1:材质着色器(MaterialShader)是一等资产 —— 原样复制;
		//     cook 指纹 = 源内容哈希 ⊕ 导入器名/版本,改代码/注解或升版就重烘,没改则跳过。
		//     规范扩展名 = `.slang`(新写路径);legacy `.hlsl` 仍可导入/烘焙(见 5c)。
		{
			const std::shared_ptr<IAssetImporter> shader = FindImporter(DefaultImporters(), "MaterialShader");
			CHECK(shader != nullptr);
			CHECK(shader->Version() == 2);
			CHECK(shader->Matches("shaders/Glow.slang"));   // 规范扩展名(向导/迁移写出的那一种)
			CHECK(shader->Matches("shaders/Glow.hlsl"));    // legacy:仍被接受(只读兼容)
			CHECK(!shader->Matches("materials/glass.wmat"));
			CHECK(!shader->Matches("scenes/a.wd"));
			CHECK(!shader->Matches("textures/x.png"));

			const std::filesystem::path content = temp.path / "shader-content";
			const std::filesystem::path source = content / "shaders" / "Glow.slang";
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
			manifest.StartScene = "shaders/Glow.slang";
			manifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path manifestPath = temp.path / "shaders.we.yaml";
			CHECK(ProjectManifest::Save(manifestPath, manifest, &error));
			const std::filesystem::path outputDir = temp.path / "shader-cooked";

			CookPipeline pipeline(DefaultImporters());
			CookSummary summary;
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 1 && summary.Changed == 1 && summary.Failed == 0);
			const std::filesystem::path artifact = outputDir / "cooked" / "shaders" / "Glow.slang";
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
			std::printf("[Slang-B1] (5b) .slang cook: 1 changed -> 1 skipped -> 1 changed (content hash)\n");
		}

		// 5c. Slang-B1:legacy `.hlsl` 仍然能被**导入 + 烘焙**(既有项目不破),
		//     并带可读迁移提示;新写路径产出 `.slang`(证据 = 5b 的 Glow.slang 全链路)。
		{
			const std::shared_ptr<IAssetImporter> shader = FindImporter(DefaultImporters(), "MaterialShader");
			CHECK(shader != nullptr);

			const std::filesystem::path legacyContent = temp.path / "legacy-shader-content";
			const std::filesystem::path legacySource = legacyContent / "shaders" / "Legacy.hlsl";
			const std::string legacyText =
				"//! param Float Roughness = 0.25 [0,1]\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    Surface surface = MakeDefaultSurface();\n"
				"    surface.Roughness = Roughness;\n"
				"    return surface;\n"
				"}\n";
			WriteBytes(legacySource, legacyText);

			ImportRequest request;
			request.LogicalPath = "shaders/Legacy.hlsl";
			request.Source = legacySource;
			std::error_code legacyEc;
			const ImportResult imported = shader->Import(request, legacyEc);
			CHECK(imported.Ok);
			CHECK(!legacyEc);
			CHECK(imported.Data.size() == legacyText.size());   // 原样复制,legacy 不改写内容
			CHECK(imported.Warnings.size() == 1);               // 可读迁移提示(结构化字段)
			CHECK(Contains(imported.Warnings[0], "legacy"));
			CHECK(Contains(imported.Warnings[0], ".slang"));    // 提示指向新扩展名
			CHECK(Contains(imported.Warnings[0], "migrate-hlsl-to-slang.py"));

			std::string error;
			ProjectManifest legacyManifest;
			legacyManifest.Id = "com.test.legacy.shaders";
			legacyManifest.ContentRoot = "legacy-shader-content";
			legacyManifest.StartScene = "shaders/Legacy.hlsl";
			legacyManifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path legacyManifestPath = temp.path / "legacy-shaders.we.yaml";
			CHECK(ProjectManifest::Save(legacyManifestPath, legacyManifest, &error));
			const std::filesystem::path legacyOutputDir = temp.path / "legacy-shader-cooked";

			CookPipeline legacyPipeline(DefaultImporters());
			CookSummary legacySummary;
			const std::vector<CookEntryResult> legacyResults =
				legacyPipeline.Cook(legacyManifest, legacyManifestPath, legacyOutputDir, false, &legacySummary);
			CHECK(legacySummary.Total == 1 && legacySummary.Changed == 1 && legacySummary.Failed == 0);
			// Slang-B1w:导入警告不再被丢弃 —— 落到条目 + 摘要(cook 摘要能读出 legacy 条目数)。
			CHECK(legacySummary.Warnings == 1 && legacySummary.WarningMessages == 1);
			CHECK(legacyResults.size() == 1 && legacyResults[0].Path == "shaders/Legacy.hlsl");
			CHECK(legacyResults[0].Warnings == imported.Warnings);
			const std::filesystem::path legacyArtifact =
				legacyOutputDir / "cooked" / "shaders" / "Legacy.hlsl";
			CHECK(ReadText(legacyArtifact) == legacyText);   // 逻辑路径与字节都不变
			std::printf("[Slang-B1] (5c) legacy .hlsl: imported + cooked unchanged, "
				"with a migration hint -> %s\n", imported.Warnings[0].c_str());
			std::printf("[Slang-B1w] (5c) cook summary: %zu/%zu asset(s) with import warnings\n",
				legacySummary.Warnings, legacySummary.Total);

			// Slang-B1w:提示随 cook.db 持久化 —— 第二次全跳过时摘要照样能报出 legacy 条目数。
			CookSummary legacySkipSummary;
			const std::vector<CookEntryResult> legacySkipResults = legacyPipeline.Cook(
				legacyManifest, legacyManifestPath, legacyOutputDir, false, &legacySkipSummary);
			CHECK(legacySkipSummary.Changed == 0 && legacySkipSummary.Skipped == 1);
			CHECK(legacySkipSummary.Warnings == 1 && legacySkipSummary.WarningMessages == 1);
			CHECK(legacySkipResults.size() == 1 && legacySkipResults[0].Warnings == imported.Warnings);
			CHECK(Contains(ReadText(legacyOutputDir / "cook.db.json"), "legacy"));
			std::printf("[Slang-B1w] (5c) skipped cook keeps the hint: %zu warning(s) "
				"(persisted in cook.db)\n", legacySkipSummary.WarningMessages);
		}

		// 5d. Slang-B1w:同一份资产 `.hlsl` → `.slang` 改名 —— 新逻辑路径必须**重烘**
		//     (不是命中旧产物),旧路径条目从 cook.db 里消失。
		{
			const std::filesystem::path content = temp.path / "rename-content";
			const std::filesystem::path oldSource = content / "shaders" / "Renamed.hlsl";
			const std::filesystem::path newSource = content / "shaders" / "Renamed.slang";
			const std::string shaderText =
				"// Slang-B1w rename fixture\n"
				"Surface Evaluate(MaterialInputs input)\n"
				"{\n"
				"    return MakeDefaultSurface();\n"
				"}\n";
			WriteBytes(oldSource, shaderText);

			std::string error;
			ProjectManifest manifest;
			manifest.Id = "com.test.rename";
			manifest.ContentRoot = "rename-content";
			manifest.StartScene = "shaders/Renamed.hlsl";
			manifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path manifestPath = temp.path / "rename.we.yaml";
			CHECK(ProjectManifest::Save(manifestPath, manifest, &error));
			const std::filesystem::path outputDir = temp.path / "rename-cooked";

			CookPipeline pipeline(DefaultImporters());
			CookSummary summary;
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 1 && summary.Changed == 1 && summary.Failed == 0);
			CHECK(ReadText(outputDir / "cooked" / "shaders" / "Renamed.hlsl") == shaderText);
			CHECK(Contains(ReadText(outputDir / "cook.db.json"), "shaders/Renamed.hlsl"));

			// 未改名:skip(对照 —— 数据库确实生效)。
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 0 && summary.Skipped == 1 && summary.Failed == 0);

			std::error_code renameEc;
			std::filesystem::rename(oldSource, newSource, renameEc);
			CHECK(!renameEc);
			pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 1 && summary.Changed == 1 && summary.Skipped == 0 && summary.Failed == 0);
			CHECK(ReadText(outputDir / "cooked" / "shaders" / "Renamed.slang") == shaderText);
			const std::string database = ReadText(outputDir / "cook.db.json");
			CHECK(Contains(database, "shaders/Renamed.slang"));
			CHECK(!Contains(database, "shaders/Renamed.hlsl"));
			std::printf("[Slang-B1w] (5d) .hlsl -> .slang rename: 1 skipped before, 1 changed after (not skipped)\n");
		}

		// 5e. Slang-B1w:v1→v2 重烘 A/B —— 同一份资产、同一逻辑路径,只有导入器版本不同:
		//     旧指纹(v1)在 cook.db 里必须失效(重烘),而不是命中旧产物。
		{
			const std::filesystem::path content = temp.path / "version-content";
			WriteBytes(content / "assets" / "thing.vsrc", "version-payload");

			std::string error;
			ProjectManifest manifest;
			manifest.Id = "com.test.versioned";
			manifest.ContentRoot = "version-content";
			manifest.StartScene = "assets/thing.vsrc";
			manifest.Packages = { "packages/Base.wpak" };
			const std::filesystem::path manifestPath = temp.path / "versioned.we.yaml";
			CHECK(ProjectManifest::Save(manifestPath, manifest, &error));
			const std::filesystem::path outputDir = temp.path / "version-cooked";

			CookPipeline v1Pipeline({ std::make_shared<VersionedCopyImporter>(1) });
			CookSummary summary;
			v1Pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Total == 1 && summary.Changed == 1 && summary.Failed == 0);
			const std::string v1Database = ReadText(outputDir / "cook.db.json");

			// 同版本再跑:skip(对照,证明数据库条目本身有效)。
			v1Pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 0 && summary.Skipped == 1 && summary.Failed == 0);

			// 只升版本(1 → 2):同一路径必须重烘,指纹前进。
			CookPipeline v2Pipeline({ std::make_shared<VersionedCopyImporter>(2) });
			v2Pipeline.Cook(manifest, manifestPath, outputDir, false, &summary);
			CHECK(summary.Changed == 1 && summary.Skipped == 0 && summary.Failed == 0);
			const std::string v2Database = ReadText(outputDir / "cook.db.json");
			CHECK(v2Database != v1Database);
			std::printf("[Slang-B1w] (5e) importer v1 -> v2: 1 skipped at v1, 1 changed at v2 (rebaked)\n");
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
