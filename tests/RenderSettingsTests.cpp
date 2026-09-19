// P1b D8a2:项目渲染设置(project.we.yaml 的 `rendering:` 区块 + 运行时生效值)。
// 覆盖:缺省、解析、写盘往返、非法值拒绝、环境变量覆盖优先级。
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Renderer/RenderSettings.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
	namespace fs = std::filesystem;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	fs::path TestRoot()
	{
		const fs::path root = fs::temp_directory_path() / "worldengine-render-settings";
		fs::remove_all(root);
		fs::create_directories(root);
		return root;
	}

	void WriteText(const fs::path& path, const std::string& text)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
	}

	std::string BaseManifest(const std::string& renderingBlock)
	{
		std::ostringstream out;
		out << "id: com.example.test\n"
			<< "version: 2.0.0\n"
			<< "content_root: assets\n"
			<< "start_scene: scenes/main.wd\n"
			<< "renderer: vulkan\n";
		out << renderingBlock;
		out << "packages:\n  - packages/Base.wpak\n";
		return out.str();
	}

	void TestDefaultsWhenBlockMissing()
	{
		const fs::path root = TestRoot();
		const fs::path manifestPath = root / "project.we.yaml";
		WriteText(manifestPath, BaseManifest(""));

		World::Asset::ProjectManifest manifest;
		std::string error;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.empty());
		CHECK(manifest.Rendering.Culling);
		CHECK(manifest.Rendering.Shadows);
		CHECK(manifest.Rendering.ShadowMapSize == 2048u);
		CHECK(manifest.Rendering.MaxDirectionalLights == 1u);
		CHECK(manifest.Rendering.MaxPointLights == 7u);
		// D8b:GPU 时间戳默认关(读回有代价);实例合批默认开。
		CHECK(!manifest.Rendering.GpuTiming);
		CHECK(manifest.Rendering.Instancing);
		// P4-1:各向异性与物理设置的默认值。
		CHECK(manifest.Rendering.Anisotropy == 1u);
		CHECK(manifest.Physics.FixedStepHz == 60u);
		CHECK(manifest.Physics.Gravity < -9.7f && manifest.Physics.Gravity > -9.9f);
		fs::remove_all(root);
	}

	void TestParsesAndRoundTrips()
	{
		const fs::path root = TestRoot();
		const fs::path manifestPath = root / "project.we.yaml";
		WriteText(manifestPath, BaseManifest(
			"rendering:\n"
			"  culling: false\n"
			"  shadows: false\n"
			"  shadow_map_size: 1024\n"
			"  max_directional_lights: 2\n"
			"  max_point_lights: 4\n"
			"  gpu_timing: true\n"
			"  instancing: false\n"
			"  anisotropy: 8\n"
			"  render_scale: 0.5\n"
			"physics:\n"
			"  fixed_step_hz: 120\n"
			"  gravity: -2.5\n"));

		World::Asset::ProjectManifest manifest;
		std::string error;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(!manifest.Rendering.Culling);
		CHECK(!manifest.Rendering.Shadows);
		CHECK(manifest.Rendering.ShadowMapSize == 1024u);
		CHECK(manifest.Rendering.MaxDirectionalLights == 2u);
		CHECK(manifest.Rendering.MaxPointLights == 4u);
		CHECK(manifest.Rendering.GpuTiming);
		CHECK(!manifest.Rendering.Instancing);
		// P4-1:各向异性 + 物理设置解析。
		CHECK(manifest.Rendering.Anisotropy == 8u);
		CHECK(manifest.Rendering.RenderScale > 0.49f && manifest.Rendering.RenderScale < 0.51f);
		CHECK(manifest.Physics.FixedStepHz == 120u);
		CHECK(manifest.Physics.Gravity < -2.4f && manifest.Physics.Gravity > -2.6f);

		// 写盘往返:rendering 区块落地,其它字段不丢。
		CHECK(World::Asset::ProjectManifest::Save(manifestPath, manifest, &error));
		World::Asset::ProjectManifest reloaded;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &reloaded, &error));
		CHECK(reloaded.Id == manifest.Id);
		CHECK(reloaded.Version == manifest.Version);
		CHECK(reloaded.StartScene == manifest.StartScene);
		CHECK(reloaded.Renderer == manifest.Renderer);
		CHECK(reloaded.Packages.size() == 1u);
		CHECK(!reloaded.Rendering.Culling);
		CHECK(!reloaded.Rendering.Shadows);
		CHECK(reloaded.Rendering.ShadowMapSize == 1024u);
		CHECK(reloaded.Rendering.MaxDirectionalLights == 2u);
		CHECK(reloaded.Rendering.MaxPointLights == 4u);
		CHECK(reloaded.Rendering.GpuTiming);
		CHECK(!reloaded.Rendering.Instancing);
		// P4-1:往返后各向异性与物理设置不丢。
		CHECK(reloaded.Rendering.Anisotropy == 8u);
		CHECK(reloaded.Rendering.RenderScale > 0.49f && reloaded.Rendering.RenderScale < 0.51f);
		CHECK(reloaded.Physics.FixedStepHz == 120u);
		CHECK(reloaded.Physics.Gravity < -2.4f && reloaded.Physics.Gravity > -2.6f);
		fs::remove_all(root);
	}

	void TestRejectsInvalidValues()
	{
		const fs::path root = TestRoot();
		const fs::path manifestPath = root / "project.we.yaml";
		World::Asset::ProjectManifest manifest;
		std::string error;

		WriteText(manifestPath, BaseManifest("rendering:\n  shadow_map_size: 1000\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.find("shadow_map_size") != std::string::npos);

		WriteText(manifestPath, BaseManifest("rendering:\n  shadow_map_size: 8192\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));

		WriteText(manifestPath, BaseManifest("rendering:\n  max_point_lights: 9\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.find("max_point_lights") != std::string::npos);

		WriteText(manifestPath, BaseManifest("rendering:\n  max_directional_lights: 3\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));

		// P4-1:各向异性越界(0 / 17)与物理越界(0 / 241)都必须被拒。
		WriteText(manifestPath, BaseManifest("rendering:\n  anisotropy: 17\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.find("anisotropy") != std::string::npos);
		WriteText(manifestPath, BaseManifest("rendering:\n  anisotropy: 0\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		WriteText(manifestPath, BaseManifest("physics:\n  fixed_step_hz: 0\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.find("fixed_step_hz") != std::string::npos);
		WriteText(manifestPath, BaseManifest("physics:\n  fixed_step_hz: 241\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		// P4-3:渲染倍率越界(0.1 / 2.5)必须被拒。
		WriteText(manifestPath, BaseManifest("rendering:\n  render_scale: 0.1\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.find("render_scale") != std::string::npos);
		WriteText(manifestPath, BaseManifest("rendering:\n  render_scale: 2.5\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));

		// 单字段不超限,但"方向光 + 点光"合计超过 UBO 容量(8)。
		WriteText(manifestPath, BaseManifest(
			"rendering:\n  max_directional_lights: 2\n  max_point_lights: 7\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.find("capacity") != std::string::npos);
		fs::remove_all(root);
	}

	void TestRuntimeApplyAndEnvironmentOverride()
	{
		World::Asset::ProjectManifest manifest;
		manifest.Id = "com.example.test";
		manifest.StartScene = "scenes/main.wd";
		manifest.Rendering.Culling = false;
		manifest.Rendering.ShadowMapSize = 512u;
		manifest.Rendering.MaxPointLights = 3u;

		_putenv_s("WLD_NO_CULL", "");
		_putenv_s("WLD_NO_SHADOWS", "");
		World::RenderSettings::Apply(manifest);
		CHECK(!World::RenderSettings::CullingEnabled());
		CHECK(World::RenderSettings::Get().ShadowMapSize == 512u);
		CHECK(World::RenderSettings::Get().MaxPointLights == 3u);

		// 环境变量是"本次运行强制关":清单里开着也要关。
		_putenv_s("WLD_NO_CULL", "1");
		_putenv_s("WLD_NO_SHADOWS", "1");
		World::Asset::ProjectManifest enabled;
		World::RenderSettings::Apply(enabled);
		CHECK(!World::RenderSettings::CullingEnabled());
		CHECK(!World::RenderSettings::ShadowsEnabled());

		// 清掉环境变量 → 回到清单值(引擎用户配置不被脚本残留影响)。
		_putenv_s("WLD_NO_CULL", "");
		_putenv_s("WLD_NO_SHADOWS", "");
		World::RenderSettings::Apply(enabled);
		CHECK(World::RenderSettings::CullingEnabled());
		CHECK(World::RenderSettings::ShadowsEnabled());
		World::RenderSettings::ResetToDefaults();
		CHECK(World::RenderSettings::Get().ShadowMapSize == 2048u);
	}
}

int main()
{
	try
	{
		TestDefaultsWhenBlockMissing();
		TestParsesAndRoundTrips();
		TestRejectsInvalidValues();
		TestRuntimeApplyAndEnvironmentOverride();
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "[FAIL] %s\n", exception.what());
		return 1;
	}
	std::printf("[PASS] World.RenderSettings\n");
	return 0;
}
