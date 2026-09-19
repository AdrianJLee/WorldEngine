#include "wldpch.h"
#include "EditorCooker.h"

#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Renderer/ShaderUtils.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <vector>

namespace World::Editor
{
	namespace fs = std::filesystem;

	namespace
	{
		// `--cook --check` 的一次性输出目录:CookPipeline 的产物与 cook.db.json 都写进这里,
		// 函数返回(含异常路径)时整体删除 —— check 因此不会触碰 build 树里的增量判定事实源。
		class CookCheckScratch
		{
		public:
			CookCheckScratch()
			{
				const fs::path tempRoot = fs::temp_directory_path();
				for (int attempt = 0; attempt < 64; ++attempt)
				{
					const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
					const fs::path candidate = tempRoot
						/ ("wld-cook-check-" + std::to_string(::GetCurrentProcessId()) + "-"
							+ std::to_string(tick) + "-" + std::to_string(attempt));
					std::error_code createEc;
					if (fs::create_directory(candidate, createEc) && !createEc)
					{
						m_Path = candidate;
						return;
					}
				}
				throw std::runtime_error("cannot create cook check scratch directory under "
					+ tempRoot.string());
			}

			~CookCheckScratch()
			{
				std::error_code removeEc;
				fs::remove_all(m_Path, removeEc);
			}

			CookCheckScratch(const CookCheckScratch&) = delete;
			CookCheckScratch& operator=(const CookCheckScratch&) = delete;

			const fs::path& Path() const { return m_Path; }

		private:
			fs::path m_Path;
		};
	}

	CookResult CookProject(const CookOptions& options)
	{
		CookResult result;
		try
		{
			// W7-2 `--check`:只做资产预检(脚本在这里就会被编译把关),不碰发行目录。
			if (options.CheckOnly)
				WLD_CORE_INFO("Check-only cook: shader bake, package, runtime copy and release manifest are skipped");
			else
				fs::create_directories(options.PublishDir);

			// 1. 项目清单(单一事实源)。
			const fs::path projectManifestPath = std::string(WLD_GAME_DIR) + "project.we.yaml";
			World::Asset::ProjectManifest manifest;
			std::string manifestError;
			if (!World::Asset::ProjectManifest::Load(projectManifestPath, &manifest, &manifestError))
				throw std::runtime_error("Project manifest load failed: " + manifestError);
			if (manifest.Packages.empty())
				throw std::runtime_error("Project manifest declares no packages");

			// 2. 启动场景覆盖(编辑器里当前打开的场景优先)。
			if (!options.StartSceneOverride.empty())
				manifest.StartScene = options.StartSceneOverride.generic_string();

			// 3. 增量资产烘焙。正常 cook 用构建期缓存目录;check 用一次性 scratch,
			//    跑完(含失败/异常)即删除,不写 cooked/、不写 cook.db.json。
			fs::path cookedDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "cooked");
			std::unique_ptr<CookCheckScratch> checkScratch;
			if (options.CheckOnly)
			{
				checkScratch = std::make_unique<CookCheckScratch>();
				cookedDir = checkScratch->Path();
				WLD_CORE_INFO("Check-only cook: artifacts go to scratch directory {0} (removed on exit)",
					cookedDir.string());
			}
			World::Asset::CookPipeline pipeline(World::Asset::DefaultImporters());
			World::Asset::CookSummary summary;
			const std::vector<World::Asset::CookEntryResult> results =
				pipeline.Cook(manifest, projectManifestPath, cookedDir, false, &summary);
			std::string failedList;
			for (const World::Asset::CookEntryResult& entry : results)
				if (entry.Failed)
				{
					WLD_CORE_ERROR("Cook failed: {0}: {1}", entry.Path, entry.Error);
					if (!failedList.empty())
						failedList += "; ";
					failedList += entry.Path + ": " + entry.Error;
				}
			WLD_CORE_INFO("Cooked {0} assets ({1} changed, {2} skipped, {3} failed)",
				summary.Total, summary.Changed, summary.Skipped, summary.Failed);
			if (summary.Failed)
				throw std::runtime_error("Asset cooking failed (" + std::to_string(summary.Failed)
					+ " failed): " + failedList);
			result.AssetsTotal = summary.Total;
			result.AssetsChanged = summary.Changed;
			result.AssetsSkipped = summary.Skipped;
			if (options.CheckOnly)
			{
				// 预检成功:摘要已填好,不创建发行目录、不烘焙着色器、不打包、不拷运行时。
				result.Ok = true;
				return result;
			}

			// 4. 着色器烘焙:引擎 HLSL → SPIR-V + GLSL 产物写入 cooked 目录,
			// 随内容包发布 —— 发行版 Runtime 不再依赖源码树与 dxc。
			const fs::path engineShaderDir = fs::absolute(std::string(WLD_WORLD_DIR) + "assets/shaders");
			const World::ShaderCompiler::BakeResult baked =
				World::ShaderCompiler::BakeDirectory(engineShaderDir, cookedDir / "cooked");
			WLD_CORE_INFO("Baked {0} shaders ({1} artifacts, {2} failed)",
				baked.Shaders, baked.Artifacts, baked.Failed);
			if (baked.Failed)
				throw std::runtime_error("Shader baking failed: " + baked.Error);
			result.ShaderArtifacts = baked.Artifacts;

			// 5. 打包 cooked 产物为发行包。
			const fs::path outPakFile = options.PublishDir / manifest.Packages[0];
			fs::create_directories(outPakFile.parent_path());
			std::error_code pakEc;
			if (!World::Vfs::PackageProvider::BuildFromDirectory(cookedDir / "cooked", outPakFile, pakEc))
				throw std::runtime_error("Package build failed: " +
					(pakEc ? pakEc.message() : outPakFile.string()));

			// 6. 拷贝运行时:Runtime.exe + 单份 WorldRuntime.dll + bin/Game.dll。
			const fs::path srcRuntimeOutputDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "Runtime/" + WLD_BUILD_TYPE);
			const fs::path srcRuntimeExe = srcRuntimeOutputDir / "Runtime.exe";
			if (!fs::is_regular_file(srcRuntimeExe))
				throw std::runtime_error("Runtime.exe could not be located: " + srcRuntimeExe.string());
			fs::copy_file(srcRuntimeExe, options.PublishDir / "Runtime.exe", fs::copy_options::overwrite_existing);
			WLD_CORE_INFO("Copied Runtime executable from: {0}", srcRuntimeExe.string());

			const fs::path srcRuntimeDll = srcRuntimeOutputDir / "WorldRuntime.dll";
			if (!fs::is_regular_file(srcRuntimeDll))
				throw std::runtime_error("WorldRuntime.dll could not be located: " + srcRuntimeDll.string());
			fs::copy_file(srcRuntimeDll, options.PublishDir / "WorldRuntime.dll", fs::copy_options::overwrite_existing);
			WLD_CORE_INFO("Copied WorldRuntime.dll from: {0}", srcRuntimeDll.string());

			const fs::path srcGameDll = fs::absolute(std::string(WLD_OUTPUT_DIR) +
				"bin/" + WLD_BUILD_TYPE + "/Game/" + WLD_BUILD_TYPE + "/Game.dll");
			if (!fs::is_regular_file(srcGameDll))
				throw std::runtime_error("Game.dll could not be located: " + srcGameDll.string());
			fs::create_directories(options.PublishDir / "bin");
			fs::copy_file(srcGameDll, options.PublishDir / "bin" / "Game.dll",
				fs::copy_options::overwrite_existing);
			WLD_CORE_INFO("Copied Game.dll into bin/");

			// 7. 写发行清单。
			std::string saveError;
			if (!World::Asset::ProjectManifest::Save(options.PublishDir / "project.we.yaml", manifest, &saveError))
				throw std::runtime_error("Project manifest save failed: " + saveError);

			WLD_CORE_INFO("Game Cooked Successfully to {0}", options.PublishDir.string());
			result.Ok = true;
		}
		catch (const std::exception& error)
		{
			result.Error = error.what();
			WLD_CORE_ERROR("Game cooking failed: {0}", result.Error);
		}
		catch (...)
		{
			result.Error = "Unknown background cooking error.";
			WLD_CORE_ERROR("{0}", result.Error);
		}
		return result;
	}
}
