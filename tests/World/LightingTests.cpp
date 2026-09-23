// P1b D4:光照系统 headless 回归。
//
// 覆盖:
//   1. 无灯光组件时环境光默认 0.25(既有场景不会突然全黑);
//   2. 上限截断:方向光 ≤1、点光 ≤7、合计 ≤8,超出的进 DroppedLights 且按顺序保留前几盏;
//   3. 方向归一化(含零向量回退 -Y);
//   4. 灯光 UBO 的 std140 布局字节数/偏移(与 Renderer3D_Solid.slang 的 cbuffer 对应);
//   5. 阴影开关:没有 CastShadow 时不启用;ApplyShadowCaster 写入矩阵并启用标志;
//   6. 组件 schema 往返(registry 的 Get/Set + 场景存档 SceneSerializer 写/读,
//      与编辑器/运行时同一条 .wd 路径)。
#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	const Schema::FieldSchema* FindField(const Schema::TypeSchema& schema, const char* name)
	{
		for (const Schema::FieldSchema& field : schema.Fields)
			if (field.Name == name)
				return &field;
		return nullptr;
	}

	bool NearlyEqual(float left, float right)
	{
		return std::fabs(left - right) < 1e-5f;
	}

	// 1.D4 前置:3D 渲染器的每帧对象上限仍可查询(打包/截断逻辑依赖它)。
	void ObjectsPerFrameLimitIsStable()
	{
		CHECK(Renderer3D::GetObjectsPerFrameLimit() > 0u);
	}

	// 2.无任何灯光组件(也没有 AmbientLightComponent)时:环境光 0.25 灰 + 强度 1,
	//   并注入一盏与 D4 之前占位实现**逐位一致**的默认主光(观感不回退);
	//   默认主光不投影,阴影通道保持禁用。
	void DefaultAmbientWithoutLights()
	{
		const LightRig rig = Renderer3D::BuildLightRig({}, {}, nullptr, /*glDepthConvention*/ false);
		CHECK(NearlyEqual(rig.Uniforms.Ambient.r, 0.25f));
		CHECK(NearlyEqual(rig.Uniforms.Ambient.g, 0.25f));
		CHECK(NearlyEqual(rig.Uniforms.Ambient.b, 0.25f));
		CHECK(NearlyEqual(rig.Uniforms.Ambient.a, 1.0f));
		CHECK(rig.Uniforms.LightCounts.x == 1u);   // 默认主光
		CHECK(rig.Uniforms.LightCounts.y == 0u);
		CHECK(rig.Uniforms.ShadowParams.x == 0.0f);   // 默认不跑阴影通道
		CHECK(rig.TotalLights == 1u);
		CHECK(rig.DroppedLights == 0u);
		CHECK(!rig.ShadowCaster);
		const LightUniforms::Light& key = rig.Uniforms.Lights[0];
		CHECK(NearlyEqual(key.PositionType.w, 1.0f));   // 方向光
		CHECK(NearlyEqual(key.ColorIntensity.r, 1.0f) && NearlyEqual(key.ColorIntensity.a, 1.0f));
		const glm::vec3 keyDirection { key.DirectionRange.x, key.DirectionRange.y, key.DirectionRange.z };
		CHECK(NearlyEqual(glm::length(keyDirection), 1.0f));
		const glm::vec3 expected = glm::normalize(glm::vec3(0.35f, -0.7f, 0.6f));
		CHECK(NearlyEqual(keyDirection.x, expected.x) && NearlyEqual(keyDirection.y, expected.y)
			&& NearlyEqual(keyDirection.z, expected.z));

		// 场景里一旦有方向光(哪怕强度 0),默认主光让位:方向就是组件给的方向。
		DirectionalLightData explicitLight;
		explicitLight.Color = { 0.5f, 0.5f, 0.5f };
		explicitLight.Intensity = 0.0f;
		explicitLight.Direction = { 0.0f, -1.0f, 0.0f };
		const LightRig replaced = Renderer3D::BuildLightRig({ explicitLight }, {}, nullptr, false);
		CHECK(replaced.Uniforms.LightCounts.x == 1u);
		CHECK(NearlyEqual(replaced.Uniforms.Lights[0].DirectionRange.y, -1.0f));
		CHECK(NearlyEqual(replaced.Uniforms.Lights[0].ColorIntensity.a, 0.0f));

		// 有 AmbientLightComponent 时逐字段采用组件值(环境色与强度分开携带)。
		AmbientLightData ambient;
		ambient.Color = { 0.2f, 0.4f, 0.6f };
		ambient.Intensity = 0.5f;
		const LightRig withAmbient = Renderer3D::BuildLightRig({}, {}, &ambient, false);
		CHECK(NearlyEqual(withAmbient.Uniforms.Ambient.r, 0.2f));
		CHECK(NearlyEqual(withAmbient.Uniforms.Ambient.g, 0.4f));
		CHECK(NearlyEqual(withAmbient.Uniforms.Ambient.b, 0.6f));
		CHECK(NearlyEqual(withAmbient.Uniforms.Ambient.a, 0.5f));
	}

	// 3.上限:方向光 1 盏、点光 7 盏、合计 8;超出部分按 registry 遍历顺序截断。
	void LightLimitsTruncateInOrder()
	{
		std::vector<DirectionalLightData> directional(3);
		for (size_t index = 0; index < directional.size(); ++index)
		{
			directional[index].Color = { static_cast<float>(index), 0.0f, 0.0f };
			directional[index].CastShadow = (index == 2);   // 第 3 盏带 CastShadow,但会被截断
		}
		std::vector<PointLightData> points(9);
		for (size_t index = 0; index < points.size(); ++index)
		{
			points[index].Position = { static_cast<float>(index), 0.0f, 0.0f };
			points[index].Range = 10.0f + static_cast<float>(index);
		}

		const LightRig rig = Renderer3D::BuildLightRig(directional, points, nullptr, false);
		CHECK(rig.Uniforms.LightCounts.x == 1u);
		CHECK(rig.Uniforms.LightCounts.y == 7u);
		CHECK(rig.TotalLights == Renderer3D::MaxLights);
		CHECK(rig.TotalLights == 8u);
		CHECK(rig.DroppedLights == 4u);   // (3 - 1) + (9 - 7)
		// 保留的是前 1 盏方向光与前 7 盏点光(registry 顺序 = 收集顺序,不重排)。
		CHECK(NearlyEqual(rig.Uniforms.Lights[0].ColorIntensity.r, 0.0f));
		CHECK(rig.Uniforms.Lights[0].PositionType.w == 1.0f);   // 方向光
		for (uint32_t index = 0; index < 7; ++index)
		{
			const LightUniforms::Light& light = rig.Uniforms.Lights[1 + index];
			CHECK(light.PositionType.w == 0.0f);                // 点光
			CHECK(NearlyEqual(light.PositionType.x, static_cast<float>(index)));
			CHECK(NearlyEqual(light.DirectionRange.x, 10.0f + static_cast<float>(index)));
		}
		// 被截断的第 3 盏方向光(CastShadow)不能把阴影打开。
		CHECK(!rig.ShadowCaster);
	}

	// 4.方向光方向做归一化;零向量回退 (0,-1,0),避免 NaN 光照。
	void DirectionalDirectionIsNormalized()
	{
		std::vector<DirectionalLightData> directional(2);
		directional[0].Direction = { 0.0f, -5.0f, 0.0f };
		directional[1].Direction = { 3.0f, -4.0f, 0.0f };

		const LightRig rig = Renderer3D::BuildLightRig(directional, {}, nullptr, false);
		CHECK(rig.Uniforms.LightCounts.x == 1u);   // 只有第一盏进入 UBO
		const glm::vec3 direction = glm::vec3(rig.Uniforms.Lights[0].DirectionRange);
		CHECK(NearlyEqual(glm::length(direction), 1.0f));
		CHECK(NearlyEqual(direction.y, -1.0f));

		std::vector<DirectionalLightData> zeroDirection(1);
		zeroDirection[0].Direction = { 0.0f, 0.0f, 0.0f };
		const LightRig zeroRig = Renderer3D::BuildLightRig(zeroDirection, {}, nullptr, false);
		const glm::vec3 fallback = glm::vec3(zeroRig.Uniforms.Lights[0].DirectionRange);
		CHECK(NearlyEqual(fallback.x, 0.0f));
		CHECK(NearlyEqual(fallback.y, -1.0f));
		CHECK(NearlyEqual(fallback.z, 0.0f));
		CHECK(NearlyEqual(glm::length(fallback), 1.0f));

		// 非轴方向也归一化(3,-4,0) → (0.6,-0.8,0)。
		std::vector<DirectionalLightData> diagonal(1);
		diagonal[0].Direction = { 3.0f, -4.0f, 0.0f };
		const glm::vec3 normalized = glm::vec3(
			Renderer3D::BuildLightRig(diagonal, {}, nullptr, false).Uniforms.Lights[0].DirectionRange);
		CHECK(NearlyEqual(normalized.x, 0.6f));
		CHECK(NearlyEqual(normalized.y, -0.8f));
	}

	// 5.std140 布局:与 Renderer3D_Solid.slang 的 cbuffer LightUniforms 逐字段对应,
	//   总大小 496(64 + 16 + 16 + 16 + 8*48)。
	void LightUniformsLayoutMatchesShader()
	{
		CHECK(sizeof(LightUniforms::Light) == 48u);
		CHECK(sizeof(LightUniforms) == 496u);
		CHECK(offsetof(LightUniforms, ShadowViewProjection) == 0u);
		CHECK(offsetof(LightUniforms, ShadowParams) == 64u);
		CHECK(offsetof(LightUniforms, Ambient) == 80u);
		CHECK(offsetof(LightUniforms, LightCounts) == 96u);
		CHECK(offsetof(LightUniforms, Lights) == 112u);

		// 默认值:阴影参数是"关闭 + 2048² + PCF 半径 1",预览/无阴影场景直接可用。
		const LightUniforms defaults;
		CHECK(defaults.ShadowParams.x == 0.0f);
		CHECK(defaults.ShadowParams.z == static_cast<float>(Renderer3D::DefaultShadowMapSize));
		CHECK(Renderer3D::DefaultShadowMapSize == 2048u);
		CHECK(Renderer3D::MaxLights == 8u);
	}

	// 6.阴影开关:只有"保留下来的主方向光 CastShadow = true"才启用;启用后写入正交矩阵。
	void ShadowCasterEnablesShadowMatrix()
	{
		std::vector<DirectionalLightData> directional(1);
		directional[0].CastShadow = true;
		LightRig rig = Renderer3D::BuildLightRig(directional, {}, nullptr, false);
		CHECK(rig.ShadowCaster);
		CHECK(rig.Uniforms.ShadowParams.x == 0.0f);   // 只有 ApplyShadowCaster 才打开

		const glm::mat4 lightViewProjection = glm::mat4(2.0f);
		Renderer3D::ApplyShadowCaster(rig, lightViewProjection);
		CHECK(rig.Uniforms.ShadowParams.x == 1.0f);
		CHECK(rig.Uniforms.ShadowViewProjection == lightViewProjection);
	}

	// 7.组件 schema 往返:registry 查找 + Get/Set + SchemaWriter(YAML)写/读。
	void ComponentSchemaRoundTrip()
	{
		WorldContext& context = TestContext();
		const Schema::TypeSchema* directional = context.Schemas().Find("World::DirectionalLightComponent");
		const Schema::TypeSchema* point = context.Schemas().Find("World::PointLightComponent");
		const Schema::TypeSchema* ambient = context.Schemas().Find("World::AmbientLightComponent");
		CHECK(directional != nullptr);
		CHECK(point != nullptr);
		CHECK(ambient != nullptr);
		// 编辑器"Add Component"枚举 TypeCategory::Component,三个组件必须都在。
		CHECK(directional->Category == Schema::TypeCategory::Component);
		CHECK(point->Category == Schema::TypeCategory::Component);
		CHECK(ambient->Category == Schema::TypeCategory::Component);
		CHECK(directional->Fields.size() == 4u);   // Color / Intensity / Direction / CastShadow
		CHECK(point->Fields.size() == 3u);         // Color / Intensity / Range
		CHECK(ambient->Fields.size() == 2u);       // Color / Intensity
		CHECK(FindField(*directional, "Color")->K == Schema::Kind::Vec3);
		CHECK(FindField(*directional, "Intensity")->K == Schema::Kind::Float);
		CHECK(FindField(*directional, "CastShadow")->K == Schema::Kind::Bool);
		CHECK(FindField(*point, "Range")->K == Schema::Kind::Float);

		World::DirectionalLightComponent light;
		const Schema::FieldSchema* intensity = FindField(*directional, "Intensity");
		intensity->Set(&light, Schema::Value(2.5f));
		CHECK(NearlyEqual(light.Intensity, 2.5f));
		CHECK(NearlyEqual(std::get<float>(intensity->Get(&light)), 2.5f));
		const Schema::FieldSchema* castShadow = FindField(*directional, "CastShadow");
		castShadow->Set(&light, Schema::Value(true));
		CHECK(light.CastShadow);
		CHECK(std::get<bool>(castShadow->Get(&light)));
		const Schema::FieldSchema* direction = FindField(*directional, "Direction");
		direction->Set(&light, Schema::Value(glm::vec3(1.0f, -2.0f, 0.25f)));
		CHECK(NearlyEqual(light.Direction.x, 1.0f));
		CHECK(NearlyEqual(light.Direction.y, -2.0f));
		CHECK(NearlyEqual(light.Direction.z, 0.25f));
		light.Color = { 0.1f, 0.2f, 0.3f };

		// 场景存档往返:与编辑器/运行时同一条 SceneSerializer(内部走 SchemaWriter/Reader)。
		const std::filesystem::path root = std::filesystem::temp_directory_path() / "we-lighting-tests";
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
		const std::filesystem::path scenePath = root / "LightingRoundTrip.wd";

		auto source = CreateRef<Scene>(context);
		{
			entt::registry& registry = source->GetRegistry();
			const entt::entity handle = registry.create();
			registry.emplace<UUIDComponent>(handle, UUID());
			registry.emplace<TagComponent>(handle, "Light Rig");
			registry.emplace<TransformComponent>(handle);
			registry.emplace<DirectionalLightComponent>(handle, light);
			auto& pointLight = registry.emplace<PointLightComponent>(handle);
			pointLight.Color = { 0.9f, 0.8f, 0.7f };
			pointLight.Intensity = 3.5f;
			pointLight.Range = 12.0f;
			auto& ambientLight = registry.emplace<AmbientLightComponent>(handle);
			ambientLight.Color = { 0.2f, 0.3f, 0.4f };
			ambientLight.Intensity = 0.5f;
		}
		{
			SceneSerializer serializer(source);
			CHECK(serializer.Serialize(scenePath.string()));
		}
		auto loaded = CreateRef<Scene>(context);
		{
			SceneSerializer serializer(loaded);
			CHECK(serializer.Deserialize(scenePath.string()));
		}
		{
			entt::registry& registry = loaded->GetRegistry();
			const auto directionalView = registry.view<DirectionalLightComponent>();
			CHECK(directionalView.size() == 1u);
			const DirectionalLightComponent& restored = registry.get<DirectionalLightComponent>(*directionalView.begin());
			CHECK(NearlyEqual(restored.Intensity, light.Intensity));
			CHECK(restored.CastShadow == light.CastShadow);
			CHECK(NearlyEqual(restored.Direction.x, light.Direction.x));
			CHECK(NearlyEqual(restored.Direction.y, light.Direction.y));
			CHECK(NearlyEqual(restored.Direction.z, light.Direction.z));
			CHECK(NearlyEqual(restored.Color.r, light.Color.r));
			CHECK(NearlyEqual(restored.Color.g, light.Color.g));
			CHECK(NearlyEqual(restored.Color.b, light.Color.b));

			const auto pointView = registry.view<PointLightComponent>();
			CHECK(pointView.size() == 1u);
			const PointLightComponent& restoredPoint = registry.get<PointLightComponent>(*pointView.begin());
			CHECK(NearlyEqual(restoredPoint.Intensity, 3.5f));
			CHECK(NearlyEqual(restoredPoint.Range, 12.0f));
			CHECK(NearlyEqual(restoredPoint.Color.b, 0.7f));

			const auto ambientView = registry.view<AmbientLightComponent>();
			CHECK(ambientView.size() == 1u);
			const AmbientLightComponent& restoredAmbient = registry.get<AmbientLightComponent>(*ambientView.begin());
			CHECK(NearlyEqual(restoredAmbient.Intensity, 0.5f));
			CHECK(NearlyEqual(restoredAmbient.Color.g, 0.3f));
		}
		std::filesystem::remove_all(root);
	}
}

int main()
{
	try
	{
		std::setvbuf(stdout, nullptr, _IONBF, 0);

		const std::pair<const char*, void(*)()> tests[] = {
			{ "Renderer3D objects-per-frame limit is queryable", ObjectsPerFrameLimitIsStable },
			{ "default ambient light is 0.25 gray without components", DefaultAmbientWithoutLights },
			{ "light limits truncate in registry order (1 directional + 7 point)", LightLimitsTruncateInOrder },
			{ "directional direction is normalized", DirectionalDirectionIsNormalized },
			{ "LightUniforms std140 layout is 496 bytes", LightUniformsLayoutMatchesShader },
			{ "CastShadow enables the shadow matrix", ShadowCasterEnablesShadowMatrix },
			{ "light component schema round-trips", ComponentSchemaRoundTrip },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		if (failures == 0)
			std::printf("World.Lighting: all checks passed\n");
		else
			std::fprintf(stderr, "World.Lighting: %d group(s) failed\n", failures);
		return failures == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Lighting: fatal: %s\n", error.what());
		return 1;
	}
}
