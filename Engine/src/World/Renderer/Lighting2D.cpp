#include "wldpch.h"
#include "World/Renderer/Lighting2D.h"
#include "World/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace World::Lighting2D
{
	namespace
	{
		float SignedPolygonArea2D(const std::vector<glm::vec2>& pts)
		{
			if (pts.size() < 3)
				return 0.0f;
			float area2 = 0.0f;
			const size_t n = pts.size();
			for (size_t i = 0; i < n; ++i)
			{
				const glm::vec2& a = pts[i];
				const glm::vec2& b = pts[(i + 1) % n];
				area2 += (a.x * b.y - b.x * a.y);
			}
			return area2 * 0.5f;
		}

		float Cross2D(const glm::vec2& a, const glm::vec2& b)
		{
			return a.x * b.y - a.y * b.x;
		}
	}

	std::vector<glm::vec2> ExtractWorldPolygon(
		const TransformComponent& transform,
		const ShadowCaster2DComponent& caster)
	{
		const std::vector<glm::vec2> localPts = caster.GetLocalPolygon();
		std::vector<glm::vec2> worldPts;
		worldPts.reserve(localPts.size());

		const glm::mat4 baseTr = glm::translate(glm::mat4(1.0f), transform.Location)
			* glm::mat4_cast(transform.Rotation)
			* glm::translate(glm::mat4(1.0f), glm::vec3(caster.Offset.x, caster.Offset.y, 0.0f));

		for (const glm::vec2& lp : localPts)
		{
			const glm::vec4 wp = baseTr * glm::vec4(lp.x * transform.Scale.x, lp.y * transform.Scale.y, 0.0f, 1.0f);
			worldPts.emplace_back(wp.x, wp.y);
		}

		if (SignedPolygonArea2D(worldPts) < 0.0f)
			std::reverse(worldPts.begin(), worldPts.end());

		return worldPts;
	}

	void ExtrudeShadowPolygon(
		const glm::vec2& lightPos,
		float lightOuterRadius,
		const std::vector<glm::vec2>& worldPolygon,
		float extrusionLength,
		float shadowIntensity,
		std::vector<ShadowQuad2D>& outQuads)
	{
		const size_t n = worldPolygon.size();
		if (n < 3 || extrusionLength <= 0.001f || lightOuterRadius <= 0.001f)
			return;

		// Quick bounding check against lightOuterRadius
		float minDistSq = std::numeric_limits<float>::max();
		for (const glm::vec2& p : worldPolygon)
		{
			const glm::vec2 d = p - lightPos;
			minDistSq = std::min(minDistSq, glm::dot(d, d));
		}
		if (minDistSq > lightOuterRadius * lightOuterRadius)
			return;

		const float clampedIntensity = glm::clamp(shadowIntensity, 0.0f, 1.0f);
		for (size_t i = 0; i < n; ++i)
		{
			const glm::vec2& a = worldPolygon[i];
			const glm::vec2& b = worldPolygon[(i + 1) % n];
			const glm::vec2 edge = b - a;
			const float edgeLen = glm::length(edge);
			if (edgeLen <= 1e-5f)
				continue;

			// Outward normal for CCW polygon is (edge.y, -edge.x)
			const glm::vec2 outwardNormal(edge.y / edgeLen, -edge.x / edgeLen);
			const glm::vec2 mid = 0.5f * (a + b);
			const glm::vec2 lightToMid = mid - lightPos;

			// Back-facing edge relative to lightPos casts extruded shadow hull
			if (glm::dot(outwardNormal, lightToMid) > 0.0f)
			{
				const glm::vec2 vecA = a - lightPos;
				const glm::vec2 vecB = b - lightPos;
				const float lenA = glm::length(vecA);
				const float lenB = glm::length(vecB);
				const glm::vec2 dirA = (lenA > 1e-5f) ? (vecA / lenA) : outwardNormal;
				const glm::vec2 dirB = (lenB > 1e-5f) ? (vecB / lenB) : outwardNormal;

				ShadowQuad2D quad;
				quad.A = a;
				quad.B = b;
				quad.BExtruded = b + dirB * extrusionLength;
				quad.AExtruded = a + dirA * extrusionLength;
				quad.ShadowIntensity = clampedIntensity;
				outQuads.push_back(quad);
			}
		}
	}

	float EvaluateRadialAttenuation(
		float distance,
		float innerRadius,
		float outerRadius,
		float falloffIntensity)
	{
		const float innerR = std::max(0.0f, innerRadius);
		const float outerR = std::max(innerR + 0.0001f, outerRadius);
		if (distance <= innerR)
			return 1.0f;
		if (distance >= outerR)
			return 0.0f;

		const float normDist = glm::clamp((distance - innerR) / (outerR - innerR), 0.0f, 1.0f);
		const float exponent = glm::mix(1.0f, 3.0f, glm::clamp(falloffIntensity, 0.0f, 1.0f));
		return std::pow(1.0f - normDist, exponent);
	}

	float EvaluateSpotAttenuation(
		const glm::vec2& lightToSampleDir,
		const glm::vec2& lightForwardDir,
		float innerAngleDegrees,
		float outerAngleDegrees)
	{
		if (outerAngleDegrees >= 359.9f)
			return 1.0f;

		const float outerClamped = glm::clamp(outerAngleDegrees, 0.1f, 360.0f);
		const float innerClamped = glm::clamp(innerAngleDegrees, 0.0f, outerClamped);
		const float cosInner = std::cos(glm::radians(innerClamped * 0.5f));
		const float cosOuter = std::cos(glm::radians(outerClamped * 0.5f));

		const float sampleLen = glm::length(lightToSampleDir);
		const float fwdLen = glm::length(lightForwardDir);
		if (sampleLen <= 1e-5f || fwdLen <= 1e-5f)
			return 1.0f;

		const float cosTheta = glm::dot(lightToSampleDir / sampleLen, lightForwardDir / fwdLen);
		if (cosTheta >= cosInner)
			return 1.0f;
		if (cosTheta <= cosOuter)
			return 0.0f;

		return glm::clamp((cosTheta - cosOuter) / std::max(cosInner - cosOuter, 0.0001f), 0.0f, 1.0f);
	}

	bool SegmentsIntersect2D(
		const glm::vec2& p0,
		const glm::vec2& p1,
		const glm::vec2& q0,
		const glm::vec2& q1)
	{
		const glm::vec2 r = p1 - p0;
		const glm::vec2 s = q1 - q0;
		const float rxs = Cross2D(r, s);
		const glm::vec2 qp = q0 - p0;

		if (std::abs(rxs) <= 1e-6f)
			return false;

		const float t = Cross2D(qp, s) / rxs;
		const float u = Cross2D(qp, r) / rxs;
		return (t > 0.001f && t < 0.999f && u >= 0.0f && u <= 1.0f);
	}

	glm::vec3 EvaluateLightingAtPoint(
		const Light2DUniforms& uniforms,
		const glm::vec2& worldPos,
		const glm::vec3& surfaceNormal,
		bool hasNormalMap,
		int32_t sortingLayer,
		int32_t entityId)
	{
		if (uniforms.GlobalDirection.w < 0.5f)
			return glm::vec3(1.0f);

		const glm::vec3 N = hasNormalMap ? glm::normalize(surfaceNormal) : glm::vec3(0.0f, 0.0f, 1.0f);
		glm::vec3 totalLight(0.0f);

		// Global ambient + optional global directional relief
		const bool globalAffectsLayer = (uniforms.Counts.z != 0) || (uniforms.Counts.w == sortingLayer);
		if (globalAffectsLayer)
		{
			glm::vec3 globalTerm = glm::vec3(uniforms.GlobalAmbient);
			if (hasNormalMap && uniforms.GlobalDirection.z > 0.001f)
			{
				const glm::vec3 globalDir3D = glm::normalize(glm::vec3(-uniforms.GlobalDirection.x, -uniforms.GlobalDirection.y, 0.6f));
				const float ndotl = std::max(glm::dot(N, globalDir3D), 0.0f);
				globalTerm = glm::mix(globalTerm, globalTerm * (0.35f + 0.95f * ndotl), glm::clamp(uniforms.GlobalDirection.z, 0.0f, 1.0f));
			}
			totalLight += globalTerm;
		}

		const int lightCount = std::clamp(uniforms.Counts.x, 0, static_cast<int>(MaxPointLights2D));
		const int segCount = std::clamp(uniforms.Counts.y, 0, static_cast<int>(MaxShadowSegments2D));

		for (int i = 0; i < lightCount; ++i)
		{
			const PointLight2DGpuData& light = uniforms.PointLights[i];
			const float targetLayer = light.Params.w;
			if (targetLayer < 50000.0f && std::abs(static_cast<float>(sortingLayer) - targetLayer) > 0.5f)
				continue;

			const glm::vec2 lightPos(light.PositionAndRadii.x, light.PositionAndRadii.y);
			const float innerRadius = light.PositionAndRadii.z;
			const float outerRadius = light.PositionAndRadii.w;

			const glm::vec2 toLight = lightPos - worldPos;
			const float dist = glm::length(toLight);
			if (dist >= outerRadius)
				continue;

			// Radial attenuation
			const float normDist = glm::clamp((dist - innerRadius) / std::max(outerRadius - innerRadius, 0.0001f), 0.0f, 1.0f);
			const float falloffExp = glm::mix(1.0f, 3.0f, glm::clamp(light.Params.x, 0.0f, 1.0f));
			const float radialAtten = (dist <= innerRadius) ? 1.0f : std::pow(1.0f - normDist, falloffExp);

			// Spot cone attenuation
			float spotAtten = 1.0f;
			const float cosInner = light.DirectionAndAngles.z;
			const float cosOuter = light.DirectionAndAngles.w;
			if (cosOuter > -0.999f && dist > 0.0001f)
			{
				const glm::vec2 lightToFragDir = -toLight / dist;
				const glm::vec2 fwdDir(light.DirectionAndAngles.x, light.DirectionAndAngles.y);
				const float cosTheta = glm::dot(lightToFragDir, fwdDir);
				spotAtten = glm::clamp((cosTheta - cosOuter) / std::max(cosInner - cosOuter, 0.0001f), 0.0f, 1.0f);
			}
			if (spotAtten <= 0.0001f)
				continue;

			// Shadow occlusion check against ShadowSegments
			float shadowVisibility = 1.0f;
			if (light.Params.z > 0.5f && segCount > 0)
			{
				float maxShadowOcclusion = 0.0f;
				for (int s = 0; s < segCount; ++s)
				{
					const ShadowSegment2DGpuData& seg = uniforms.ShadowSegments[s];
					const int casterEntityId = static_cast<int>( std::round(seg.Params.y) );
					const bool selfShadows = (seg.Params.z > 0.5f);
					if (entityId >= 0 && casterEntityId == entityId && !selfShadows)
						continue;

					const glm::vec2 q0(seg.Endpoints.x, seg.Endpoints.y);
					const glm::vec2 q1(seg.Endpoints.z, seg.Endpoints.w);
					if (SegmentsIntersect2D(worldPos, lightPos, q0, q1))
					{
						maxShadowOcclusion = std::max(maxShadowOcclusion, glm::clamp(seg.Params.x, 0.0f, 1.0f));
					}
				}
				shadowVisibility = 1.0f - maxShadowOcclusion;
			}
			if (shadowVisibility <= 0.0001f)
				continue;

			// Normal map Lambertian diffuse term
			float diffuse = 1.0f;
			if (hasNormalMap)
			{
				const float normalZ = std::max(light.Params.y, 0.05f);
				const glm::vec3 lightVec3D = glm::normalize(glm::vec3(toLight.x, toLight.y, normalZ));
				diffuse = std::max(glm::dot(N, lightVec3D), 0.0f);
			}

			const glm::vec3 lightColor(light.ColorAndIntensity.x, light.ColorAndIntensity.y, light.ColorAndIntensity.z);
			const float lightIntensity = light.ColorAndIntensity.w;
			totalLight += lightColor * (lightIntensity * radialAtten * spotAtten * shadowVisibility * diffuse);
		}

		return totalLight;
	}

	Light2DUniforms BuildSceneLight2DUniforms(const Scene* scene)
	{
		Light2DUniforms uniforms{};
		if (!scene)
			return uniforms;

		const auto& registry = scene->GetRegistry();

		bool hasAnyGlobalLight = false;
		for (auto entity : registry.view<GlobalLight2DComponent>())
		{
			if (!scene->IsEntityInActiveDataLayers(entity))
				continue;
			const auto& gl = registry.get<GlobalLight2DComponent>(entity);
			if (!gl.Enabled)
				continue;

			uniforms.GlobalAmbient = glm::vec4(gl.Color * std::max(gl.Intensity, 0.0f), 0.0f);
			glm::vec2 dir = gl.Direction;
			const float dirLen = glm::length(dir);
			if (dirLen > 1e-5f)
				dir /= dirLen;
			else
				dir = glm::vec2(0.0f, -1.0f);

			uniforms.GlobalDirection = glm::vec4(dir.x, dir.y, glm::clamp(gl.DirectionIntensity, 0.0f, 1.0f), 1.0f);
			uniforms.Counts.z = gl.AffectAllLayers ? 1 : 0;
			uniforms.Counts.w = gl.TargetSortingLayer;
			hasAnyGlobalLight = true;
			break;
		}

		int pointLightCount = 0;
		for (auto entity : registry.view<TransformComponent, PointLight2DComponent>())
		{
			if (!scene->IsEntityInActiveDataLayers(entity))
				continue;
			const auto& transform = registry.get<TransformComponent>(entity);
			const auto& pl = registry.get<PointLight2DComponent>(entity);
			if (!pl.Enabled || pl.Intensity <= 0.0f || pl.OuterRadius <= 0.0f)
				continue;
			if (pointLightCount >= static_cast<int>(MaxPointLights2D))
				break;

			PointLight2DGpuData& dst = uniforms.PointLights[pointLightCount++];
			const float innerR = std::max(0.0f, pl.InnerRadius);
			const float outerR = std::max(innerR + 0.001f, pl.OuterRadius);
			dst.PositionAndRadii = glm::vec4(transform.Location.x, transform.Location.y, innerR, outerR);
			dst.ColorAndIntensity = glm::vec4(pl.Color, pl.Intensity);

			const float rotZ = transform.GetEulerAngles().z;
			const glm::vec2 fwd(-std::sin(rotZ), std::cos(rotZ));
			const float outerAng = glm::clamp(pl.OuterAngle, 0.1f, 360.0f);
			const float innerAng = glm::clamp(pl.InnerAngle, 0.0f, outerAng);
			const float cosInner = (outerAng >= 359.9f) ? -1.0f : std::cos(glm::radians(innerAng * 0.5f));
			const float cosOuter = (outerAng >= 359.9f) ? -1.0f : std::cos(glm::radians(outerAng * 0.5f));
			dst.DirectionAndAngles = glm::vec4(fwd.x, fwd.y, cosInner, cosOuter);

			dst.Params = glm::vec4(
				glm::clamp(pl.FalloffIntensity, 0.0f, 1.0f),
				std::max(pl.NormalMapZ, 0.05f),
				pl.CastShadows ? 1.0f : 0.0f,
				pl.AffectAllLayers ? 99999.0f : static_cast<float>(pl.TargetSortingLayer));
		}

		uniforms.Counts.x = pointLightCount;
		if (!hasAnyGlobalLight && pointLightCount == 0)
		{
			// No 2D lights in scene -> keep 2D lighting disabled (100% unlit passthrough)
			uniforms.GlobalAmbient = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
			uniforms.GlobalDirection = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
			return uniforms;
		}

		if (!hasAnyGlobalLight && pointLightCount > 0)
		{
			// Point lights exist without an explicit GlobalLight2D -> provide a soft night ambient
			uniforms.GlobalAmbient = glm::vec4(0.18f, 0.19f, 0.24f, 0.0f);
			uniforms.GlobalDirection = glm::vec4(0.0f, -1.0f, 0.0f, 1.0f);
			uniforms.Counts.z = 1;
			uniforms.Counts.w = 0;
		}

		// Collect shadow caster silhouette segments
		int segCount = 0;
		for (auto entity : registry.view<TransformComponent, ShadowCaster2DComponent>())
		{
			if (!scene->IsEntityInActiveDataLayers(entity))
				continue;
			const auto& transform = registry.get<TransformComponent>(entity);
			const auto& caster = registry.get<ShadowCaster2DComponent>(entity);
			if (!caster.CastShadows || caster.ShadowIntensity <= 0.001f)
				continue;

			const std::vector<glm::vec2> poly = ExtractWorldPolygon(transform, caster);
			if (poly.size() < 3)
				continue;

			const size_t n = poly.size();
			for (size_t i = 0; i < n; ++i)
			{
				if (segCount >= static_cast<int>(MaxShadowSegments2D))
					break;
				const glm::vec2& a = poly[i];
				const glm::vec2& b = poly[(i + 1) % n];
				if (glm::length(b - a) <= 1e-5f)
					continue;

				ShadowSegment2DGpuData& seg = uniforms.ShadowSegments[segCount++];
				seg.Endpoints = glm::vec4(a.x, a.y, b.x, b.y);
				seg.Params = glm::vec4(
					glm::clamp(caster.ShadowIntensity, 0.0f, 1.0f),
					static_cast<float>(static_cast<uint32_t>(entity)),
					caster.SelfShadows ? 1.0f : 0.0f,
					caster.ExtrusionLength);
			}
			if (segCount >= static_cast<int>(MaxShadowSegments2D))
				break;
		}
		uniforms.Counts.y = segCount;
		return uniforms;
	}
}
