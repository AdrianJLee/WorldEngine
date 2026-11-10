#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	class Scene;

	namespace Lighting2D
	{
		constexpr uint32_t MaxPointLights2D = 16;
		constexpr uint32_t MaxShadowSegments2D = 32;
		constexpr int32_t UnlitSortingLayerSentinel = -99999;

		// std140 GPU/CPU point light entry (64 bytes)
		struct alignas(16) PointLight2DGpuData
		{
			glm::vec4 PositionAndRadii { 0.0f, 0.0f, 0.5f, 5.0f };       // xy = world pos, z = InnerRadius, w = OuterRadius
			glm::vec4 ColorAndIntensity { 1.0f, 1.0f, 1.0f, 1.0f };      // rgb = linear color, w = Intensity
			glm::vec4 DirectionAndAngles { 0.0f, 1.0f, -1.0f, -1.0f };   // xy = forward dir, z = cos(Inner*0.5), w = cos(Outer*0.5)
			glm::vec4 Params { 0.5f, 0.5f, 1.0f, 99999.0f };             // x = FalloffIntensity, y = NormalMapZ, z = CastShadows(1/0), w = TargetSortingLayer (99999 = all)
		};
		static_assert(sizeof(PointLight2DGpuData) == 64, "PointLight2DGpuData must be 64 bytes (std140)");

		// std140 GPU/CPU shadow occluder segment entry (32 bytes)
		struct alignas(16) ShadowSegment2DGpuData
		{
			glm::vec4 Endpoints { 0.0f, 0.0f, 0.0f, 0.0f }; // xy = A, zw = B in world 2D space
			glm::vec4 Params { 0.85f, -1.0f, 0.0f, 12.0f }; // x = ShadowIntensity, y = float(EntityID), z = SelfShadows(1/0), w = ExtrusionLength
		};
		static_assert(sizeof(ShadowSegment2DGpuData) == 32, "ShadowSegment2DGpuData must be 32 bytes (std140)");

		// std140 GPU Constant Buffer bound at set=1, binding=4 in Renderer2D_Quad.slang (2096 bytes)
		struct alignas(16) Light2DUniforms
		{
			glm::vec4 GlobalAmbient { 1.0f, 1.0f, 1.0f, 0.0f };   // rgb = global color * intensity, w = unused
			glm::vec4 GlobalDirection { 0.0f, -1.0f, 0.0f, 0.0f }; // xy = normalized 2D dir, z = DirectionIntensity, w = LightingEnabled (0 or 1)
			glm::ivec4 Counts { 0, 0, 1, 0 };                      // x = PointLightCount, y = ShadowSegmentCount, z = GlobalAffectAllLayers, w = GlobalTargetSortingLayer
			PointLight2DGpuData PointLights[MaxPointLights2D];
			ShadowSegment2DGpuData ShadowSegments[MaxShadowSegments2D];
		};
		static_assert(sizeof(Light2DUniforms) == 48 + 64 * MaxPointLights2D + 32 * MaxShadowSegments2D,
			"Light2DUniforms std140 size mismatch");

		// Extruded 2D shadow quad (trapezoid from silhouette edge A->B to extruded AExtruded->BExtruded)
		struct ShadowQuad2D
		{
			glm::vec2 A { 0.0f, 0.0f };
			glm::vec2 B { 0.0f, 0.0f };
			glm::vec2 BExtruded { 0.0f, 0.0f };
			glm::vec2 AExtruded { 0.0f, 0.0f };
			float ShadowIntensity = 0.85f;
		};

		// Extract world-space CCW 2D polygon hull from TransformComponent + ShadowCaster2DComponent
		WLD_API std::vector<glm::vec2> ExtractWorldPolygon(
			const TransformComponent& transform,
			const ShadowCaster2DComponent& caster);

		// Extract world-space CCW 2D polygon hull from TransformComponent + FreeformLight2DComponent
		WLD_API std::vector<glm::vec2> ExtractWorldPolygon(
			const TransformComponent& transform,
			const FreeformLight2DComponent& light);

		struct FreeformLight2DInstance
		{
			entt::entity Entity { entt::null };
			glm::vec4 Color { 1.0f, 1.0f, 0.9f, 1.0f };
			float Intensity = 1.0f;
			float FalloffDistance = 0.5f;
			int32_t TargetSortingLayer = 0;
			bool AffectAllLayers = true;
			bool CastShadows = false;
			std::vector<glm::vec2> WorldPolygon;
		};

		// Collect all active FreeformLight2D instances from the scene
		WLD_API std::vector<FreeformLight2DInstance> CollectFreeformLights2D(const Scene* scene);

		// Extrude silhouette/back-facing edges of a world-space 2D polygon away from lightPos
		WLD_API void ExtrudeShadowPolygon(
			const glm::vec2& lightPos,
			float lightOuterRadius,
			const std::vector<glm::vec2>& worldPolygon,
			float extrusionLength,
			float shadowIntensity,
			std::vector<ShadowQuad2D>& outQuads);

		// Radial distance attenuation in [0, 1] for a 2D point light
		WLD_API float EvaluateRadialAttenuation(
			float distance,
			float innerRadius,
			float outerRadius,
			float falloffIntensity);

		// Spot cone angular attenuation in [0, 1] for a 2D light
		WLD_API float EvaluateSpotAttenuation(
			const glm::vec2& lightToSampleDir,
			const glm::vec2& lightForwardDir,
			float innerAngleDegrees,
			float outerAngleDegrees);

		// 2D segment-segment intersection test (determines if ray from samplePos to lightPos is blocked by segment q0->q1)
		WLD_API bool SegmentsIntersect2D(
			const glm::vec2& p0,
			const glm::vec2& p1,
			const glm::vec2& q0,
			const glm::vec2& q1);

		// Evaluate total 2D lighting color (ambient + point lights + normal map relief + shadow occlusion) at a world 2D point
		WLD_API glm::vec3 EvaluateLightingAtPoint(
			const Light2DUniforms& uniforms,
			const glm::vec2& worldPos,
			const glm::vec3& surfaceNormal = glm::vec3(0.0f, 0.0f, 1.0f),
			bool hasNormalMap = false,
			int32_t sortingLayer = 0,
			int32_t entityId = -1);

		// Build Light2DUniforms from all active GlobalLight2D, PointLight2D, and ShadowCaster2D entities in a Scene
		WLD_API Light2DUniforms BuildSceneLight2DUniforms(const Scene* scene);
	}
}
