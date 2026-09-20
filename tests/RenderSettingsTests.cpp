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
#include <vector>

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

	std::string ReadText(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		std::ostringstream buffer;
		buffer << file.rdbuf();
		return buffer.str();
	}

	// 按行切开(去掉行尾符),用于"只有哪几行变了"的断言。
	std::vector<std::string> SplitLinesOf(const std::string& text)
	{
		std::vector<std::string> lines;
		std::string current;
		for (const char c : text)
		{
			if (c == '\n')
			{
				lines.push_back(current);
				current.clear();
			}
			else if (c != '\r')
				current += c;
		}
		if (!current.empty())
			lines.push_back(current);
		return lines;
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
		// P4-perf:垂直同步默认开(游戏防撕裂;关闭走 Immediate 呈现)。
		CHECK(manifest.Rendering.Vsync);
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
			"  vsync: false\n"
			"  instancing: false\n"
			"  anisotropy: 8\n"
			"  render_scale: 0.5\n"
			"  msaa: 4\n"
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
		CHECK(!manifest.Rendering.Vsync);
		CHECK(!manifest.Rendering.Instancing);
		// P4-1:各向异性 + 物理设置解析。
		CHECK(manifest.Rendering.Anisotropy == 8u);
		CHECK(manifest.Rendering.RenderScale > 0.49f && manifest.Rendering.RenderScale < 0.51f);
		CHECK(manifest.Rendering.Msaa == 4u);
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
		CHECK(!reloaded.Rendering.Vsync);
		CHECK(!reloaded.Rendering.Instancing);
		// P4-1:往返后各向异性与物理设置不丢。
		CHECK(reloaded.Rendering.Anisotropy == 8u);
		CHECK(reloaded.Rendering.RenderScale > 0.49f && reloaded.Rendering.RenderScale < 0.51f);
		CHECK(reloaded.Rendering.Msaa == 4u);
		CHECK(reloaded.Physics.FixedStepHz == 120u);
		CHECK(reloaded.Physics.Gravity < -2.4f && reloaded.Physics.Gravity > -2.6f);
		fs::remove_all(root);
	}

	// U2e:设置面板是"改一个开关 → 400ms 防抖自动写盘",不能再整份重排清单 ——
	// 清单里的注释是给引擎用户看的产品资产,改动之外的行必须逐字节不动。
	void TestSaveKeepsCommentsAndUnchangedLines()
	{
		const fs::path root = TestRoot();
		const fs::path manifestPath = root / "project.we.yaml";
		// 与 Game/project.we.yaml 同构:区块前的中文注释 + 规范数值 + packages 序列。
		const std::string original =
			"id: com.example.test\n"
			"version: 2.0.0\n"
			"content_root: assets\n"
			"start_scene: scenes/main.wd\n"
			"renderer: vulkan\n"
			"# 3D 渲染设置(引擎用户可直接改)\n"
			"rendering:\n"
			"  culling: true\n"
			"  shadows: true   # 方向光阴影通道\n"
			"  shadow_map_size: 2048\n"
			"  max_directional_lights: 1\n"
			"  max_point_lights: 7\n"
			"  gpu_timing: false\n"
			"  vsync: true\n"
			"  instancing: true\n"
			"  anisotropy: 1\n"
			"  render_scale: 1.0\n"
			"  msaa: 1\n"
			"# 物理设置(引擎用户可直接改)\n"
			"physics:\n"
			"  fixed_step_hz: 60\n"
			"  gravity: -9.81\n"
			"# 发行包\n"
			"packages:\n"
			"  - packages/Base.wpak\n";
		WriteText(manifestPath, original);

		World::Asset::ProjectManifest manifest;
		std::string error;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.empty());

		// 1) 值没变:写盘结果与原文件逐字节一致(注释、排版、数值写法都不动)。
		CHECK(World::Asset::ProjectManifest::Save(manifestPath, manifest, &error));
		CHECK(ReadText(manifestPath) == original);

		// 2) 改两个值(开关取反 + 重力):只有这两行变,其余行逐字节一致。
		manifest.Rendering.Shadows = false;
		manifest.Physics.Gravity = -2.5f;
		CHECK(World::Asset::ProjectManifest::Save(manifestPath, manifest, &error));
		const std::string merged = ReadText(manifestPath);

		std::string expected = original;
		const std::string shadowFrom = "  shadows: true   # 方向光阴影通道\n";
		const size_t shadowAt = expected.find(shadowFrom);
		CHECK(shadowAt != std::string::npos);
		expected.replace(shadowAt, shadowFrom.size(), "  shadows: false   # 方向光阴影通道\n");
		const std::string gravityFrom = "  gravity: -9.81\n";
		const size_t gravityAt = expected.find(gravityFrom);
		CHECK(gravityAt != std::string::npos);
		expected.replace(gravityAt, gravityFrom.size(), "  gravity: -2.5\n");
		CHECK(merged == expected);

		// 3) 关键点单独断言(失败时比整串比较好定位)。
		CHECK(merged.find("# 3D 渲染设置(引擎用户可直接改)\n") != std::string::npos);
		CHECK(merged.find("# 物理设置(引擎用户可直接改)\n") != std::string::npos);
		CHECK(merged.find("# 发行包\n") != std::string::npos);
		// a) 行尾注释与缩进保留;b) 只有被改的那一行变化。
		CHECK(merged.find("  shadows: false   # 方向光阴影通道\n") != std::string::npos);
		CHECK(merged.find("  gravity: -2.5\n") != std::string::npos);
		// c) 未动的行逐字节一致:render_scale 不能被写成 `1`,gravity 也不能变成 -9.810000。
		CHECK(merged.find("  render_scale: 1.0\n") != std::string::npos);
		CHECK(merged.find("  culling: true\n") != std::string::npos);
		CHECK(merged.find("  - packages/Base.wpak\n") != std::string::npos);
		CHECK(merged.find("-9.810000") == std::string::npos);
		CHECK(merged.find("render_scale: 1\n") == std::string::npos);

		// 4) 行序与行数不变,且差异行数正好是 2。
		const std::vector<std::string> before = SplitLinesOf(original);
		const std::vector<std::string> after = SplitLinesOf(merged);
		CHECK(before.size() == after.size());
		size_t changed = 0;
		for (size_t i = 0; i < before.size(); ++i)
			if (before[i] != after[i])
				++changed;
		CHECK(changed == 2u);

		// 5) 合并后的清单仍能读回同样的值。
		World::Asset::ProjectManifest reloaded;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &reloaded, &error));
		CHECK(!reloaded.Rendering.Shadows);
		CHECK(reloaded.Rendering.Culling);
		CHECK(reloaded.Rendering.RenderScale > 0.99f && reloaded.Rendering.RenderScale < 1.01f);
		CHECK(reloaded.Rendering.Msaa == 1u);
		CHECK(reloaded.Physics.FixedStepHz == 60u);
		CHECK(reloaded.Physics.Gravity < -2.4f && reloaded.Physics.Gravity > -2.6f);
		CHECK(reloaded.Packages.size() == 1u);
		fs::remove_all(root);
	}

	// U2e:文件里缺的 key/区块要被补上(而不是丢),缺的 rendering key 追加在所属区块内。
	void TestSaveAppendsMissingKeys()
	{
		const fs::path root = TestRoot();
		const fs::path manifestPath = root / "project.we.yaml";
		WriteText(manifestPath,
			"id: com.example.test\n"
			"content_root: assets\n"
			"start_scene: scenes/main.wd\n"
			"# 渲染设置\n"
			"rendering:\n"
			"  shadows: false\n");

		World::Asset::ProjectManifest manifest;
		std::string error;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		manifest.Packages.push_back("packages/Base.wpak");   // 文件里没有 packages 区块 → 补一整块
		CHECK(World::Asset::ProjectManifest::Save(manifestPath, manifest, &error));
		const std::string text = ReadText(manifestPath);

		// 原有注释与原有行都没被动。
		CHECK(text.find("# 渲染设置\n") != std::string::npos);
		CHECK(text.find("  shadows: false\n") != std::string::npos);
		// 缺的 rendering key 按清单顺序追加在 rendering 区块末尾(2 空格缩进,仍在区块内)。
		CHECK(text.find(
			"  shadows: false\n"
			"  culling: true\n"
			"  shadow_map_size: 2048\n"
			"  max_directional_lights: 1\n"
			"  max_point_lights: 7\n"
			"  gpu_timing: false\n"
			"  vsync: true\n"
			"  instancing: true\n"
			"  anisotropy: 1\n"
			"  msaa: 1\n"
			"  render_scale: 1.0\n") != std::string::npos);
		// 缺的顶层键追加到文件末尾;缺的 physics / packages 区块整块补上。
		CHECK(text.find("version: 1.0.0\n") != std::string::npos);
		CHECK(text.find("renderer: opengl\n") != std::string::npos);
		CHECK(text.find("physics:\n  fixed_step_hz: 60\n  gravity: -9.81\n") != std::string::npos);
		CHECK(text.find("packages:\n  - packages/Base.wpak\n") != std::string::npos);
		// 追加后仍是合法清单,值与原对象一致。
		World::Asset::ProjectManifest reloaded;
		CHECK(World::Asset::ProjectManifest::Load(manifestPath, &reloaded, &error));
		CHECK(!reloaded.Rendering.Shadows);
		CHECK(reloaded.Rendering.Culling);
		CHECK(reloaded.Rendering.ShadowMapSize == 2048u);
		CHECK(reloaded.Rendering.Msaa == 1u);
		CHECK(reloaded.Rendering.RenderScale > 0.99f && reloaded.Rendering.RenderScale < 1.01f);
		CHECK(reloaded.Physics.FixedStepHz == 60u);
		CHECK(reloaded.Physics.Gravity < -9.7f && reloaded.Physics.Gravity > -9.9f);
		CHECK(reloaded.Packages.size() == 1u && reloaded.Packages[0] == "packages/Base.wpak");
		CHECK(reloaded.Renderer == "opengl");
		CHECK(reloaded.Version == "1.0.0");
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
		// P4-4:MSAA 只允许 1/2/4/8(3 与 16 必须被拒)。
		WriteText(manifestPath, BaseManifest("rendering:\n  msaa: 3\n"));
		CHECK(!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error));
		CHECK(error.find("msaa") != std::string::npos);
		WriteText(manifestPath, BaseManifest("rendering:\n  msaa: 16\n"));
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
		TestSaveKeepsCommentsAndUnchangedLines();
		TestSaveAppendsMissingKeys();
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
