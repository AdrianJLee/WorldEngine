// P4-U4(2026-09-21):两个"设置作用域"的 headless 回归。
//
//   1. Asset Import Defaults:`project.we.yaml` 的 `imports:`(= ModelImportSettings)
//      - 解析 / 文本合并保存(注释与未受管 key 不动、默认值不往清单里塞块);
//      - `ModelImportSettings::Load` 的优先级:同目录 `.wimport` > 项目默认 > 引擎默认;
//      - `LoadProjectDefaults(manifestPath)`:读到清单就生效,清单缺失/坏掉回到引擎默认。
//   2. World / Scene Settings:`.wd` 头部的 `World:` 块(= Scene::WorldSettings)
//      - 默认值不写块(旧文件形态不变);
//      - 写了 physics_gravity / physics_debug → 往返读回一致;
//      - 非法值(非有限重力)不吞:落到"未覆盖",并保留另一项。
#include "wldpch.h"
#include "World/Core/Asset/ModelImportSettings.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		CHECK(static_cast<bool>(stream));
		return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	void WriteText(const std::filesystem::path& path, const std::string& text)
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		CHECK(static_cast<bool>(stream));
		stream << text;
	}

	// 一份"带注释、带未受管 key"的清单:合并保存后这些必须原样还在。
	const char* kManifestText =
		"# 项目清单(注释必须活下来)\n"
		"id: u4-test\n"
		"version: \"1.0.0\"\n"
		"content_root: assets\n"
		"start_scene: scenes/3DTest.wd\n"
		"renderer: vulkan\n"
		"custom_key: keep-me\n"
		"physics:\n"
		"  fixed_step_hz: 60\n"
		"  gravity: -9.81   # 行尾注释\n"
		"packages:\n"
		"  - packages/Base.wpak\n";
}

int main()
{
	try
	{
		const std::filesystem::path root = std::filesystem::temp_directory_path() / "we-u4-settings-tests";
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
		std::filesystem::create_directories(root, ignored);
		CHECK(std::filesystem::is_directory(root));

		// ---- 1. 清单:默认不写 imports:,非默认写进去且能读回 ----
		const std::filesystem::path manifestPath = root / "project.we.yaml";
		WriteText(manifestPath, kManifestText);
		{
			World::Asset::ProjectManifest manifest;
			std::string error;
			CHECK(World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
			CHECK(error.empty());
			// 缺 `imports:` = 引擎默认。
			CHECK(World::Asset::ModelImportSettings::Hash(manifest.ImportDefaults)
				== World::Asset::ModelImportSettings::Hash(World::Asset::ModelImportSettings::Default()));

			// 只改一个导入默认值 → 合并保存:注释 / 行尾注释 / 未受管 key 必须原样。
			manifest.ImportDefaults.UpAxis = 1;
			manifest.ImportDefaults.Scale = 2.5f;
			CHECK(World::Asset::ProjectManifest::Save(manifestPath, manifest, &error));
			const std::string saved = ReadText(manifestPath);
			CHECK(saved.find("# 项目清单(注释必须活下来)") != std::string::npos);
			CHECK(saved.find("gravity: -9.81   # 行尾注释") != std::string::npos);
			CHECK(saved.find("custom_key: keep-me") != std::string::npos);
			CHECK(saved.find("imports:") != std::string::npos);
			CHECK(saved.find("model.up_axis: \"Z\"") != std::string::npos);
		}
		{
			// 读回:字段值与刚才写的一致。
			World::Asset::ProjectManifest manifest;
			std::string error;
			CHECK(World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
			CHECK(manifest.ImportDefaults.UpAxis == 1);
			CHECK(std::fabs(manifest.ImportDefaults.Scale - 2.5f) < 1e-4f);
			CHECK(manifest.ImportDefaults.ExportMaterials);
			CHECK(std::fabs(manifest.ImportDefaults.AnimationSampleRate - 30.0f) < 1e-4f);
		}
		{
			// 非法 up_axis 必须在加载期拒绝(不静默改成 Y)。
			const std::filesystem::path badPath = root / "bad-axis.yaml";
			WriteText(badPath, std::string(kManifestText) + "imports:\n  model.up_axis: \"X\"\n");
			World::Asset::ProjectManifest manifest;
			std::string error;
			CHECK(!World::Asset::ProjectManifest::Load(badPath, &manifest, &error));
			CHECK(error.find("up_axis") != std::string::npos);
		}
		{
			// 非法 sample rate 同样拒绝(0 会让导入期按 0 步长烘动画)。
			const std::filesystem::path badRate = root / "bad-rate.yaml";
			WriteText(badRate, std::string(kManifestText) + "imports:\n  model.animation_sample_rate: 0\n");
			World::Asset::ProjectManifest manifest;
			std::string error;
			CHECK(!World::Asset::ProjectManifest::Load(badRate, &manifest, &error));
			CHECK(error.find("animation_sample_rate") != std::string::npos);
		}

		// ---- 2. 导入默认值的优先级 ----
		{
			// 清单存在 → 进程级默认生效。
			CHECK(World::Asset::ModelImportSettings::LoadProjectDefaults(manifestPath.string()));
			CHECK(World::Asset::ModelImportSettings::ProjectDefaults().UpAxis == 1);
			CHECK(std::fabs(World::Asset::ModelImportSettings::ProjectDefaults().Scale - 2.5f) < 1e-4f);

			// 源没有 .wimport → 用项目默认。
			const std::filesystem::path source = root / "models" / "NoSidecar.gltf";
			std::filesystem::create_directories(source.parent_path(), ignored);
			WriteText(source, "{}");
			const World::Asset::ModelImportSettings loaded =
				World::Asset::ModelImportSettings::Load(source.string());
			CHECK(loaded.UpAxis == 1);
			CHECK(std::fabs(loaded.Scale - 2.5f) < 1e-4f);

			// 源有 .wimport → **旁路文件优先**于项目默认。
			const std::filesystem::path sidecar = root / "models" / "WithSidecar.wimport";
			WriteText(sidecar, "{\"scale\": 0.5, \"upAxis\": \"Y\"}");
			const std::filesystem::path sidecarSource = root / "models" / "WithSidecar.gltf";
			WriteText(sidecarSource, "{}");
			const World::Asset::ModelImportSettings overridden =
				World::Asset::ModelImportSettings::Load(sidecarSource.string());
			CHECK(overridden.UpAxis == 0);
			CHECK(std::fabs(overridden.Scale - 0.5f) < 1e-4f);
		}
		{
			// 清单缺失 → 回到引擎默认(不能让上一个项目的默认值泄漏)。
			CHECK(!World::Asset::ModelImportSettings::LoadProjectDefaults((root / "nope.yaml").string()));
			CHECK(World::Asset::ModelImportSettings::Hash(World::Asset::ModelImportSettings::ProjectDefaults())
				== World::Asset::ModelImportSettings::Hash(World::Asset::ModelImportSettings::Default()));
		}

		// ---- 2b. 注释模板:引擎生成的清单必须自带说明(用户:「注释说明你可以想个办法存起来」)----
		{
			// 全量序列化(文件不存在):头部说明 + 每个区块的说明都要写出来。
			const std::filesystem::path freshPath = root / "fresh" / "project.we.yaml";
			std::filesystem::create_directories(freshPath.parent_path(), ignored);
			World::Asset::ProjectManifest manifest;
			manifest.Id = "fresh";
			manifest.StartScene = "scenes/3DTest.wd";
			std::string error;
			CHECK(World::Asset::ProjectManifest::Save(freshPath, manifest, &error));
			const std::string fresh = ReadText(freshPath);
			CHECK(fresh.find("# WorldEngine 项目清单") != std::string::npos);
			CHECK(fresh.find("# 渲染:") != std::string::npos);
			CHECK(fresh.find("# 物理:") != std::string::npos);
		}
		{
			// 文本合并路径:原文件**没有** imports: 且导入默认值非默认 → 补块时必须带上该块的说明。
			const std::filesystem::path mergedPath = root / "merged" / "project.we.yaml";
			std::filesystem::create_directories(mergedPath.parent_path(), ignored);
			WriteText(mergedPath, kManifestText);
			World::Asset::ProjectManifest manifest;
			std::string error;
			CHECK(World::Asset::ProjectManifest::Load(mergedPath, &manifest, &error));
			manifest.ImportDefaults.GenerateNormals = false;   // 非默认 → 触发补块
			CHECK(World::Asset::ProjectManifest::Save(mergedPath, manifest, &error));
			const std::string merged = ReadText(mergedPath);
			CHECK(merged.find("# 资产导入默认值") != std::string::npos);
			CHECK(merged.find("model.generate_normals: false") != std::string::npos);
			// 用户原有的注释与未受管 key 仍然原样。
			CHECK(merged.find("# 项目清单(注释必须活下来)") != std::string::npos);
			CHECK(merged.find("custom_key: keep-me") != std::string::npos);
		}

		// ---- 3. .wd 头部的 World 块 ----
		World::WorldContext context;
		const std::filesystem::path scenePath = root / "WorldSettings.wd";
		{
			auto scene = CreateRef<Scene>(context);
			// 默认:不写 World 块。
			{
				SceneSerializer serializer(scene);
				CHECK(serializer.Serialize(scenePath.string()));
			}
			const std::string text = ReadText(scenePath);
			CHECK(text.find("World:") == std::string::npos);

			// 非默认:physics_debug + 重力覆盖都写进去。
			scene->GetWorldSettings().PhysicsDebug = true;
			scene->GetWorldSettings().Gravity = -3.5f;
			SceneSerializer serializer(scene);
			CHECK(serializer.Serialize(scenePath.string()));
			const std::string withWorld = ReadText(scenePath);
			CHECK(withWorld.find("World:") != std::string::npos);
			CHECK(withWorld.find("physics_debug: true") != std::string::npos);
			CHECK(withWorld.find("physics_gravity: -3.5") != std::string::npos);
		}
		{
			auto loaded = CreateRef<Scene>(context);
			SceneSerializer serializer(loaded);
			CHECK(serializer.Deserialize(scenePath.string()));
			const Scene::WorldSettings& settings = loaded->GetWorldSettings();
			CHECK(settings.PhysicsDebug);
			CHECK(settings.HasGravityOverride());
			CHECK(std::fabs(settings.Gravity - (-3.5f)) < 1e-4f);
			CHECK(!settings.IsDefault());
		}
		{
			// 非法重力:落到"未覆盖",但 physics_debug 仍生效(不因为一个坏字段丢整块)。
			const std::filesystem::path badScene = root / "BadWorld.wd";
			const std::string text =
				"FormatVersion: 2\n"
				"Scene: Untitled\n"
				"World:\n"
				"  physics_gravity: .nan\n"
				"  physics_debug: true\n"
				"Entities: []\n";
			WriteText(badScene, text);
			auto loaded = CreateRef<Scene>(context);
			SceneSerializer serializer(loaded);
			CHECK(serializer.Deserialize(badScene.string()));
			CHECK(!loaded->GetWorldSettings().HasGravityOverride());
			CHECK(loaded->GetWorldSettings().PhysicsDebug);
		}
		{
			// 旧场景(没有 World 块)读进来 = 全默认。
			const std::filesystem::path legacy = root / "Legacy.wd";
			WriteText(legacy, "FormatVersion: 2\nScene: Untitled\nEntities: []\n");
			auto loaded = CreateRef<Scene>(context);
			SceneSerializer serializer(loaded);
			CHECK(serializer.Deserialize(legacy.string()));
			CHECK(loaded->GetWorldSettings().IsDefault());
		}

		std::filesystem::remove_all(root, ignored);
		std::printf("World.ImportWorldSettings: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.ImportWorldSettings: FAILED: %s\n", error.what());
		return 1;
	}
}
