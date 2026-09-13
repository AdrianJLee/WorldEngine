#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Vfs/PackageProvider.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
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
}

int main()
{
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

		std::printf("World.Asset: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Asset: FAILED: %s\n", error.what());
		return 1;
	}
}
