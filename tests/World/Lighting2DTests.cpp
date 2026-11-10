#include "World/Renderer/Lighting2D.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Entity.h"
#include "World/Core/StringPool.h"
#include "World/Core/Log.h"

#include <iostream>
#include <cassert>
#include <cmath>

using namespace World;

static bool NearlyEqual(float a, float b, float eps = 1e-4f)
{
	return std::abs(a - b) <= eps;
}

static float ComputeSignedArea(const std::vector<glm::vec2>& poly)
{
	if (poly.size() < 3)
		return 0.0f;
	float area2 = 0.0f;
	for (size_t i = 0; i < poly.size(); ++i)
	{
		const glm::vec2& a = poly[i];
		const glm::vec2& b = poly[(i + 1) % poly.size()];
		area2 += (a.x * b.y - b.x * a.y);
	}
	return area2 * 0.5f;
}

static void TestComponentLayoutAndDefaults()
{
	static_assert(sizeof(PointLight2DComponent) == 48, "PointLight2DComponent must be 48 bytes");
	static_assert(sizeof(GlobalLight2DComponent) == 36, "GlobalLight2DComponent must be 36 bytes");
	static_assert(sizeof(ShadowCaster2DComponent) == 32, "ShadowCaster2DComponent must be 32 bytes");
	static_assert(sizeof(SpriteComponent) == 64, "SpriteComponent must be 64 bytes");

	static_assert(std::is_trivially_copyable_v<PointLight2DComponent>, "PointLight2DComponent must be trivially copyable");
	static_assert(std::is_trivially_copyable_v<GlobalLight2DComponent>, "GlobalLight2DComponent must be trivially copyable");
	static_assert(std::is_trivially_copyable_v<ShadowCaster2DComponent>, "ShadowCaster2DComponent must be trivially copyable");
	static_assert(std::is_trivially_copyable_v<SpriteComponent>, "SpriteComponent must be trivially copyable");

	PointLight2DComponent pl;
	assert(NearlyEqual(pl.Color.r, 1.0f) && NearlyEqual(pl.Color.g, 1.0f) && NearlyEqual(pl.Color.b, 1.0f));
	assert(NearlyEqual(pl.Intensity, 1.0f));
	assert(NearlyEqual(pl.InnerRadius, 0.5f));
	assert(NearlyEqual(pl.OuterRadius, 5.0f));
	assert(NearlyEqual(pl.FalloffIntensity, 0.5f));
	assert(NearlyEqual(pl.InnerAngle, 360.0f));
	assert(NearlyEqual(pl.OuterAngle, 360.0f));
	assert(NearlyEqual(pl.NormalMapZ, 0.5f));
	assert(pl.TargetSortingLayer == 0);
	assert(pl.CastShadows == true);
	assert(pl.AffectAllLayers == true);
	assert(pl.Enabled == true);

	GlobalLight2DComponent gl;
	assert(NearlyEqual(gl.Color.r, 1.0f) && NearlyEqual(gl.Color.g, 1.0f) && NearlyEqual(gl.Color.b, 1.0f));
	assert(NearlyEqual(gl.Intensity, 1.0f));
	assert(NearlyEqual(gl.Direction.x, 0.0f) && NearlyEqual(gl.Direction.y, -1.0f));
	assert(NearlyEqual(gl.DirectionIntensity, 0.0f));
	assert(gl.TargetSortingLayer == 0);
	assert(gl.AffectAllLayers == true);
	assert(gl.Enabled == true);

	ShadowCaster2DComponent sc;
	assert(NearlyEqual(sc.Offset.x, 0.0f) && NearlyEqual(sc.Offset.y, 0.0f));
	assert(NearlyEqual(sc.Size.x, 1.0f) && NearlyEqual(sc.Size.y, 1.0f));
	assert(!sc.Points.IsValid());
	assert(NearlyEqual(sc.ShadowIntensity, 0.85f));
	assert(NearlyEqual(sc.ExtrusionLength, 12.0f));
	assert(sc.ShapeType == 0);
	assert(sc.SelfShadows == false);
	assert(sc.CastShadows == true);
	assert(sc.ShowGizmo == true);

	SpriteComponent sp;
	assert(NearlyEqual(sp.Color.r, 1.0f) && NearlyEqual(sp.Color.a, 1.0f));
	assert(NearlyEqual(sp.Pivot.x, 0.5f) && NearlyEqual(sp.Pivot.y, 0.5f));
	assert(sp.SortingLayer == 0 && sp.OrderInLayer == 0);
	assert(!sp.NormalMap.IsValid());

	std::cout << "[PASS] TestComponentLayoutAndDefaults\n";
}

static void TestRadialAndSpotAttenuation()
{
	// 1. EvaluateRadialAttenuation
	{
		const float innerR = 1.0f;
		const float outerR = 5.0f;
		assert(NearlyEqual(Lighting2D::EvaluateRadialAttenuation(0.0f, innerR, outerR, 0.5f), 1.0f));
		assert(NearlyEqual(Lighting2D::EvaluateRadialAttenuation(1.0f, innerR, outerR, 0.5f), 1.0f));
		assert(NearlyEqual(Lighting2D::EvaluateRadialAttenuation(5.0f, innerR, outerR, 0.5f), 0.0f));
		assert(NearlyEqual(Lighting2D::EvaluateRadialAttenuation(8.0f, innerR, outerR, 0.5f), 0.0f));

		// Midpoint (distance = 3.0 -> normDist = 0.5)
		const float linearMid = Lighting2D::EvaluateRadialAttenuation(3.0f, innerR, outerR, 0.0f);
		const float quadMid = Lighting2D::EvaluateRadialAttenuation(3.0f, innerR, outerR, 0.5f);
		const float steepMid = Lighting2D::EvaluateRadialAttenuation(3.0f, innerR, outerR, 1.0f);
		assert(NearlyEqual(linearMid, 0.5f));
		assert(NearlyEqual(quadMid, 0.25f));
		assert(NearlyEqual(steepMid, 0.125f));
		assert(linearMid > quadMid && quadMid > steepMid);
	}

	// 2. EvaluateSpotAttenuation
	{
		const glm::vec2 fwd(0.0f, 1.0f);
		// 360 degree omnidirectional point light returns 1.0 in any direction
		assert(NearlyEqual(Lighting2D::EvaluateSpotAttenuation(glm::vec2(0.0f, 1.0f), fwd, 360.0f, 360.0f), 1.0f));
		assert(NearlyEqual(Lighting2D::EvaluateSpotAttenuation(glm::vec2(1.0f, 0.0f), fwd, 360.0f, 360.0f), 1.0f));
		assert(NearlyEqual(Lighting2D::EvaluateSpotAttenuation(glm::vec2(0.0f, -1.0f), fwd, 360.0f, 360.0f), 1.0f));

		// Cone spotlight: InnerAngle = 30 deg (half = 15 deg), OuterAngle = 90 deg (half = 45 deg)
		assert(NearlyEqual(Lighting2D::EvaluateSpotAttenuation(glm::vec2(0.0f, 1.0f), fwd, 30.0f, 90.0f), 1.0f));

		// 10 deg off-axis (within inner cone half-angle 15 deg) -> 1.0f
		const float rad10 = glm::radians(10.0f);
		const glm::vec2 dir10(std::sin(rad10), std::cos(rad10));
		assert(NearlyEqual(Lighting2D::EvaluateSpotAttenuation(dir10, fwd, 30.0f, 90.0f), 1.0f));

		// 30 deg off-axis (between 15 deg and 45 deg) -> smooth falloff in (0, 1)
		const float rad30 = glm::radians(30.0f);
		const glm::vec2 dir30(std::sin(rad30), std::cos(rad30));
		const float spotMid = Lighting2D::EvaluateSpotAttenuation(dir30, fwd, 30.0f, 90.0f);
		assert(spotMid > 0.0f && spotMid < 1.0f);

		// 60 deg off-axis (outside outer cone half-angle 45 deg) -> 0.0f
		const float rad60 = glm::radians(60.0f);
		const glm::vec2 dir60(std::sin(rad60), std::cos(rad60));
		assert(NearlyEqual(Lighting2D::EvaluateSpotAttenuation(dir60, fwd, 30.0f, 90.0f), 0.0f));
	}

	std::cout << "[PASS] TestRadialAndSpotAttenuation\n";
}

static void TestShadowPolygonExtractionAndExtrusion()
{
	TransformComponent tr;
	tr.Location = { 2.0f, 1.0f, 0.0f };
	tr.Scale = { 2.0f, 2.0f, 1.0f };

	ShadowCaster2DComponent caster;
	caster.Size = { 1.0f, 1.0f };
	caster.ShapeType = 0; // Box
	caster.ShadowIntensity = 0.8f;
	caster.ExtrusionLength = 10.0f;

	// Extract world-space CCW polygon (2x2 box centered at (2, 1) -> [1..3] x [0..2])
	std::vector<glm::vec2> worldPoly = Lighting2D::ExtractWorldPolygon(tr, caster);
	assert(worldPoly.size() == 4);
	assert(ComputeSignedArea(worldPoly) > 0.0f); // Must be CCW

	// Also verify clockwise custom polygon is automatically normalized to CCW
	ShadowCaster2DComponent customCaster;
	customCaster.ShapeType = 1;
	customCaster.Points = StringPool::Get().InternName("-1,-1; -1,1; 1,1; 1,-1"); // CW order
	std::vector<glm::vec2> customPoly = Lighting2D::ExtractWorldPolygon(TransformComponent{}, customCaster);
	assert(customPoly.size() == 4);
	assert(ComputeSignedArea(customPoly) > 0.0f);

	// Extrude shadow away from a light at (-2, 1) (to the left of the [1..3]x[0..2] box)
	std::vector<Lighting2D::ShadowQuad2D> quads;
	Lighting2D::ExtrudeShadowPolygon(
		glm::vec2(-2.0f, 1.0f),
		15.0f,
		worldPoly,
		caster.ExtrusionLength,
		caster.ShadowIntensity,
		quads);

	// For a axis-aligned box with light strictly to its left (-2, 1), top/bottom/right edges face away from the light
	assert(!quads.empty());
	for (const auto& q : quads)
	{
		assert(NearlyEqual(q.ShadowIntensity, 0.8f));
		const float lenA = glm::length(q.AExtruded - q.A);
		const float lenB = glm::length(q.BExtruded - q.B);
		assert(NearlyEqual(lenA, 10.0f, 1e-3f));
		assert(NearlyEqual(lenB, 10.0f, 1e-3f));
		// Extruded points must be further from the light than base vertices
		assert(glm::length(q.AExtruded - glm::vec2(-2.0f, 1.0f)) > glm::length(q.A - glm::vec2(-2.0f, 1.0f)));
	}

	// Out-of-range light produces 0 shadow quads
	std::vector<Lighting2D::ShadowQuad2D> farQuads;
	Lighting2D::ExtrudeShadowPolygon(
		glm::vec2(-100.0f, 1.0f),
		5.0f,
		worldPoly,
		caster.ExtrusionLength,
		caster.ShadowIntensity,
		farQuads);
	assert(farQuads.empty());

	std::cout << "[PASS] TestShadowPolygonExtractionAndExtrusion\n";
}

static void TestSegmentsIntersectAndEvaluateLighting()
{
	// 1. SegmentsIntersect2D
	assert(Lighting2D::SegmentsIntersect2D(
		glm::vec2(0.0f, 0.0f), glm::vec2(4.0f, 0.0f),
		glm::vec2(2.0f, -1.0f), glm::vec2(2.0f, 1.0f)));
	assert(!Lighting2D::SegmentsIntersect2D(
		glm::vec2(0.0f, 0.0f), glm::vec2(1.5f, 0.0f),
		glm::vec2(2.0f, -1.0f), glm::vec2(2.0f, 1.0f)));

	// 2. EvaluateLightingAtPoint: unlit passthrough when lighting disabled
	Lighting2D::Light2DUniforms uniforms{};
	uniforms.GlobalDirection.w = 0.0f;
	const glm::vec3 unlit = Lighting2D::EvaluateLightingAtPoint(uniforms, glm::vec2(0.0f, 0.0f));
	assert(NearlyEqual(unlit.r, 1.0f) && NearlyEqual(unlit.g, 1.0f) && NearlyEqual(unlit.b, 1.0f));

	// Enable lighting with low ambient and 1 warm point light at (0, 0)
	uniforms.GlobalAmbient = glm::vec4(0.1f, 0.1f, 0.1f, 0.0f);
	uniforms.GlobalDirection = glm::vec4(0.0f, -1.0f, 0.5f, 1.0f); // Enabled = 1
	uniforms.Counts = glm::ivec4(1, 1, 1, 0);

	auto& pl = uniforms.PointLights[0];
	pl.PositionAndRadii = glm::vec4(0.0f, 0.0f, 0.5f, 10.0f);
	pl.ColorAndIntensity = glm::vec4(1.0f, 0.8f, 0.4f, 2.0f);
	pl.DirectionAndAngles = glm::vec4(0.0f, 1.0f, -1.0f, -1.0f);
	pl.Params = glm::vec4(0.5f, 0.5f, 1.0f, 99999.0f); // CastShadows = 1, all layers

	// Place a vertical shadow caster segment at x = 2.0, y in [-2, 2], belonging to entityId = 42
	auto& seg = uniforms.ShadowSegments[0];
	seg.Endpoints = glm::vec4(2.0f, -2.0f, 2.0f, 2.0f);
	seg.Params = glm::vec4(0.85f, 42.0f, 0.0f, 12.0f); // ShadowIntensity = 0.85, EntityID = 42, SelfShadows = 0

	// Point in front of occluder (x = 1.0, y = 0.0) is fully lit by point light
	const glm::vec3 litFront = Lighting2D::EvaluateLightingAtPoint(uniforms, glm::vec2(1.0f, 0.0f));
	// Point behind occluder (x = 4.0, y = 0.0) is occluded by segment at x = 2.0
	const glm::vec3 litBehindOccluded = Lighting2D::EvaluateLightingAtPoint(uniforms, glm::vec2(4.0f, 0.0f), glm::vec3(0, 0, 1), false, 0, 99);
	// Same point (x = 4.0, y = 0.0) evaluated for entityId = 42 with SelfShadows = false is NOT self-occluded!
	const glm::vec3 litBehindSelfNoShadow = Lighting2D::EvaluateLightingAtPoint(uniforms, glm::vec2(4.0f, 0.0f), glm::vec3(0, 0, 1), false, 0, 42);

	assert(litFront.r > litBehindOccluded.r);
	assert(litBehindSelfNoShadow.r > litBehindOccluded.r);

	// Normal map relief test at (-1.5, 0.0) (light is at (0,0), so toLight is +X)
	const glm::vec3 normalFacingLight = glm::normalize(glm::vec3(0.8f, 0.0f, 0.6f));
	const glm::vec3 normalFacingAway = glm::normalize(glm::vec3(-0.8f, 0.0f, 0.6f));
	const glm::vec3 litFacing = Lighting2D::EvaluateLightingAtPoint(uniforms, glm::vec2(-1.5f, 0.0f), normalFacingLight, true);
	const glm::vec3 litAway = Lighting2D::EvaluateLightingAtPoint(uniforms, glm::vec2(-1.5f, 0.0f), normalFacingAway, true);
	assert(litFacing.r > litAway.r);

	// Sorting layer filtering test: restrict point light to SortingLayer = 3
	pl.Params.w = 3.0f;
	const glm::vec3 layer0Lit = Lighting2D::EvaluateLightingAtPoint(uniforms, glm::vec2(-1.0f, 0.0f), glm::vec3(0, 0, 1), false, 0);
	const glm::vec3 layer3Lit = Lighting2D::EvaluateLightingAtPoint(uniforms, glm::vec2(-1.0f, 0.0f), glm::vec3(0, 0, 1), false, 3);
	assert(NearlyEqual(layer0Lit.r, 0.1f)); // Only ambient on layer 0
	assert(layer3Lit.r > 1.0f);             // Ambient + point light on layer 3

	std::cout << "[PASS] TestSegmentsIntersectAndEvaluateLighting\n";
}

static void TestBuildSceneLight2DUniforms()
{
	WorldContext context;
	Scene scene(context);

	// Empty scene has lighting disabled (GlobalDirection.w == 0)
	{
		const Lighting2D::Light2DUniforms emptyUbo = Lighting2D::BuildSceneLight2DUniforms(&scene);
		assert(NearlyEqual(emptyUbo.GlobalDirection.w, 0.0f));
		assert(emptyUbo.Counts.x == 0 && emptyUbo.Counts.y == 0);
	}

	// Populate scene with GlobalLight2D, 2 PointLight2Ds (omnidirectional + cone), and 1 ShadowCaster2D
	Entity globalEntity = Entity::CreateEntity(&scene, "GlobalLight2D_Atmosphere");
	globalEntity.AddComponent<TransformComponent>();
	auto& gl = globalEntity.AddComponent<GlobalLight2DComponent>();
	gl.Color = { 0.4f, 0.5f, 0.6f };
	gl.Intensity = 0.8f;
	gl.Direction = { 0.0f, -1.0f };
	gl.DirectionIntensity = 0.35f;

	Entity torchEntity = Entity::CreateEntity(&scene, "TorchLight2D_Warm");
	torchEntity.AddComponent<TransformComponent>().Location = { -2.0f, 1.5f, 0.0f };
	auto& torch = torchEntity.AddComponent<PointLight2DComponent>();
	torch.Color = { 1.0f, 0.65f, 0.25f };
	torch.Intensity = 2.2f;
	torch.InnerRadius = 0.6f;
	torch.OuterRadius = 7.5f;
	torch.CastShadows = true;

	Entity spotEntity = Entity::CreateEntity(&scene, "Flashlight2D_Cone");
	spotEntity.AddComponent<TransformComponent>().Location = { 3.0f, -1.0f, 0.0f };
	auto& spot = spotEntity.AddComponent<PointLight2DComponent>();
	spot.Color = { 0.5f, 0.8f, 1.0f };
	spot.Intensity = 1.6f;
	spot.InnerAngle = 30.0f;
	spot.OuterAngle = 65.0f;
	spot.OuterRadius = 6.5f;

	Entity pillarEntity = Entity::CreateEntity(&scene, "ShadowPillar2D_Obstacle");
	pillarEntity.AddComponent<TransformComponent>().Location = { 0.5f, 0.5f, 0.0f };
	auto& caster = pillarEntity.AddComponent<ShadowCaster2DComponent>();
	caster.Size = { 1.2f, 1.2f };
	caster.ShadowIntensity = 0.85f;
	caster.ExtrusionLength = 12.0f;
	caster.CastShadows = true;

	const Lighting2D::Light2DUniforms ubo = Lighting2D::BuildSceneLight2DUniforms(&scene);
	assert(NearlyEqual(ubo.GlobalDirection.w, 1.0f)); // Lighting enabled
	assert(NearlyEqual(ubo.GlobalAmbient.r, 0.4f * 0.8f));
	assert(NearlyEqual(ubo.GlobalAmbient.g, 0.5f * 0.8f));
	assert(NearlyEqual(ubo.GlobalAmbient.b, 0.6f * 0.8f));
	assert(NearlyEqual(ubo.GlobalDirection.z, 0.35f));

	assert(ubo.Counts.x == 2); // 2 active point lights
	assert(ubo.Counts.y == 4); // 4 silhouette segments from box ShadowCaster2D

	std::cout << "[PASS] TestBuildSceneLight2DUniforms\n";
}

int main()
{
	Log::Init();
	TestComponentLayoutAndDefaults();
	TestRadialAndSpotAttenuation();
	TestShadowPolygonExtractionAndExtrusion();
	TestSegmentsIntersectAndEvaluateLighting();
	TestBuildSceneLight2DUniforms();
	std::cout << "All Phase 9 Lighting2D tests passed!\n";
	return 0;
}
