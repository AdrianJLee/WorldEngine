#pragma once

#include <vector>
#include <unordered_set>
#include <string>
#include <cmath>
#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <entt.hpp>

#include "World/Scene/Components.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/Tilemap.h"
#include "World/Renderer/FrustumCull.h"

namespace World
{
	struct SceneOccluder
	{
		glm::vec3 WorldMin { 0.0f };
		glm::vec3 WorldMax { 0.0f };
		glm::mat4 InvWorldMatrix { 1.0f };
		glm::vec3 LocalMin { 0.0f };
		glm::vec3 LocalMax { 0.0f };
		entt::entity Owner { entt::null };
		bool HasInvMatrix { false };
	};

	inline bool RayHitsAABB(const glm::vec3& orig, const glm::vec3& dir, float maxDist, const glm::vec3& bMin, const glm::vec3& bMax)
	{
		float tmin = 0.0f;
		float tmax = maxDist;
		for (int i = 0; i < 3; ++i)
		{
			if (std::abs(dir[i]) < 1e-6f)
			{
				if (orig[i] < bMin[i] || orig[i] > bMax[i]) return false;
			}
			else
			{
				const float ood = 1.0f / dir[i];
				float t1 = (bMin[i] - orig[i]) * ood;
				float t2 = (bMax[i] - orig[i]) * ood;
				if (t1 > t2) std::swap(t1, t2);
				tmin = std::max(tmin, t1);
				tmax = std::min(tmax, t2);
				if (tmin > tmax) return false;
			}
		}
		return tmin > 0.05f && tmin < maxDist - 0.05f;
	}

	inline bool RayHitsSceneOccluders(const std::vector<SceneOccluder>& occluders,
		const glm::vec3& orig, const glm::vec3& normDir, float maxDist,
		const glm::vec3& targetPt, entt::entity ignoreEntity = entt::null)
	{
		if (occluders.empty() || maxDist < 0.5f)
			return false;

		for (const auto& occ : occluders)
		{
			if (ignoreEntity != entt::null && occ.Owner == ignoreEntity)
				continue;

			// 宽相粗筛：射线与世界 AABB 求交
			if (!RayHitsAABB(orig, normDir, maxDist, occ.WorldMin, occ.WorldMax))
			{
				const bool targetInsideWorld = (targetPt.x >= occ.WorldMin.x && targetPt.x <= occ.WorldMax.x &&
					targetPt.y >= occ.WorldMin.y && targetPt.y <= occ.WorldMax.y &&
					targetPt.z >= occ.WorldMin.z && targetPt.z <= occ.WorldMax.z);
				if (!targetInsideWorld)
					continue;
			}

			// 窄相精测：变换至局部空间进行精准 OBB 判定
			if (occ.HasInvMatrix)
			{
				const glm::vec3 localOrig   = glm::vec3(occ.InvWorldMatrix * glm::vec4(orig, 1.0f));
				const glm::vec3 localDir    = glm::vec3(occ.InvWorldMatrix * glm::vec4(normDir, 0.0f));
				const glm::vec3 localTarget = glm::vec3(occ.InvWorldMatrix * glm::vec4(targetPt, 1.0f));

				const bool targetInside = (localTarget.x >= occ.LocalMin.x && localTarget.x <= occ.LocalMax.x &&
					localTarget.y >= occ.LocalMin.y && localTarget.y <= occ.LocalMax.y &&
					localTarget.z >= occ.LocalMin.z && localTarget.z <= occ.LocalMax.z);
				if (targetInside)
				{
					const bool camInside = (localOrig.x >= occ.LocalMin.x && localOrig.x <= occ.LocalMax.x &&
						localOrig.y >= occ.LocalMin.y && localOrig.y <= occ.LocalMax.y &&
						localOrig.z >= occ.LocalMin.z && localOrig.z <= occ.LocalMax.z);
					if (!camInside)
						return true;
				}
				if (RayHitsAABB(localOrig, localDir, maxDist, occ.LocalMin, occ.LocalMax))
					return true;
			}
			else
			{
				const bool targetInside = (targetPt.x >= occ.WorldMin.x && targetPt.x <= occ.WorldMax.x &&
					targetPt.y >= occ.WorldMin.y && targetPt.y <= occ.WorldMax.y &&
					targetPt.z >= occ.WorldMin.z && targetPt.z <= occ.WorldMax.z);
				if (targetInside)
				{
					const bool camInside = (orig.x >= occ.WorldMin.x && orig.x <= occ.WorldMax.x &&
						orig.y >= occ.WorldMin.y && orig.y <= occ.WorldMax.y &&
						orig.z >= occ.WorldMin.z && orig.z <= occ.WorldMax.z);
					if (!camInside)
						return true;
				}
				if (RayHitsAABB(orig, normDir, maxDist, occ.WorldMin, occ.WorldMax))
					return true;
			}
		}
		return false;
	}

	inline std::vector<SceneOccluder> CollectSceneOccluders(const entt::registry& registry, entt::entity selectedHandle = entt::null)
	{
		std::vector<SceneOccluder> occluders;
		std::unordered_set<entt::entity> visited;

		auto getWorldMatrix = [&](entt::entity h, const TransformComponent& xf) -> glm::mat4 {
			if (const auto* wt = registry.try_get<WorldTransformComponent>(h))
				return wt->Matrix;
			return xf.GetLocalMatrix();
		};

		// 1. 3D 网格模型 (MeshRendererComponent)
		for (auto [h, xf, mr] : registry.view<TransformComponent, MeshRendererComponent>().each())
		{
			if (mr.Primitive == MeshRendererComponent::PrimitiveShape::Plane && !mr.Mesh.HasPath() && !mr.Mesh.Identity.IsValid())
				continue;

			const glm::mat4 world = getWorldMatrix(h, xf);
			glm::vec3 localMin { -0.5f, -0.5f, -0.5f };
			glm::vec3 localMax {  0.5f,  0.5f,  0.5f };
			if (mr.Mesh.HasPath() || mr.Mesh.Identity.IsValid())
			{
				std::string err;
				if (auto mesh = Mesh::LoadWModel(mr.Mesh.Path, &err))
				{
					localMin = mesh->GetBounds().Min;
					localMax = mesh->GetBounds().Max;
				}
			}

			if (localMin.x > localMax.x || localMin.y > localMax.y || localMin.z > localMax.z)
			{
				localMin = glm::vec3(-0.5f);
				localMax = glm::vec3(0.5f);
			}
			localMax = glm::max(localMax, localMin + glm::vec3(0.05f));

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);
			const glm::vec3 ext = (worldMax - worldMin) * 0.5f;
			if (ext.y < 0.6f && (ext.x > 4.0f || ext.z > 4.0f))
				continue;
			if (ext.x < 0.10f && ext.y < 0.10f && ext.z < 0.10f)
				continue;

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		// 2. 动画骨骼模型 (SkinnedMeshRendererComponent)
		for (auto [h, xf, smr] : registry.view<TransformComponent, SkinnedMeshRendererComponent>().each())
		{
			if (visited.count(h))
				continue;

			const glm::mat4 world = getWorldMatrix(h, xf);
			glm::vec3 localMin { -0.5f, -0.5f, -0.5f };
			glm::vec3 localMax {  0.5f,  0.5f,  0.5f };
			if (smr.Mesh.HasPath() || smr.Mesh.Identity.IsValid())
			{
				std::string err;
				if (auto mesh = Mesh::LoadWModel(smr.Mesh.Path, &err))
				{
					localMin = mesh->GetBounds().Min;
					localMax = mesh->GetBounds().Max;
				}
			}

			if (localMin.x > localMax.x || localMin.y > localMax.y || localMin.z > localMax.z)
			{
				localMin = glm::vec3(-0.5f);
				localMax = glm::vec3(0.5f);
			}
			localMax = glm::max(localMax, localMin + glm::vec3(0.05f));

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);
			const glm::vec3 ext = (worldMax - worldMin) * 0.5f;
			if (ext.y < 0.6f && (ext.x > 4.0f || ext.z > 4.0f))
				continue;
			if (ext.x < 0.10f && ext.y < 0.10f && ext.z < 0.10f)
				continue;

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		// 3. 2D 图像与精灵 (SpriteComponent)
		for (auto [h, xf, spr] : registry.view<TransformComponent, SpriteComponent>().each())
		{
			if (visited.count(h))
				continue;

			glm::mat4 world = getWorldMatrix(h, xf);
			if (spr.Pivot.x != 0.5f || spr.Pivot.y != 0.5f)
			{
				world = glm::translate(world, glm::vec3(0.5f - spr.Pivot.x, 0.5f - spr.Pivot.y, 0.0f));
			}

			const glm::vec3 localMin { -0.5f, -0.5f, -0.15f };
			const glm::vec3 localMax {  0.5f,  0.5f,  0.15f };

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);
			const glm::vec3 ext = (worldMax - worldMin) * 0.5f;
			if (ext.x < 0.10f && ext.y < 0.10f && ext.z < 0.10f)
				continue;

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		// 4. 瓦片地图 (TilemapComponent)
		for (auto [h, xf, tmComp] : registry.view<TransformComponent, TilemapComponent>().each())
		{
			if (visited.count(h))
				continue;

			const glm::mat4 world = getWorldMatrix(h, xf);
			float mapW = 16.0f, mapH = 16.0f;
			if (tmComp.Tilemap.HasPath())
			{
				const std::string tmP = StringPool::Get().PathOf(tmComp.Tilemap.Path);
				if (auto tm = Tilemap::GetOrLoad(tmP))
				{
					mapW = static_cast<float>(tm->GetWidth()) * (tmComp.CellSize.x > 0.0f ? tmComp.CellSize.x : tm->GetCellSize().x);
					mapH = static_cast<float>(tm->GetHeight()) * (tmComp.CellSize.y > 0.0f ? tmComp.CellSize.y : tm->GetCellSize().y);
				}
			}

			const glm::vec3 localMin { 0.0f, 0.0f, -0.15f };
			const glm::vec3 localMax { mapW, mapH,  0.15f };

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		// 5. 白盒几何体原型 (WhiteboxComponent)
		for (auto [h, xf, wb] : registry.view<TransformComponent, WhiteboxComponent>().each())
		{
			if (visited.count(h))
				continue;

			const glm::mat4 world = getWorldMatrix(h, xf);
			const glm::vec3 localMin = -wb.Dimensions * 0.5f;
			const glm::vec3 localMax =  wb.Dimensions * 0.5f;

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);
			const glm::vec3 ext = (worldMax - worldMin) * 0.5f;
			if (ext.y < 0.6f && (ext.x > 4.0f || ext.z > 4.0f))
				continue;
			if (ext.x < 0.10f && ext.y < 0.10f && ext.z < 0.10f)
				continue;

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		// 6. 3D 盒体碰撞体 (BoxCollider3DComponent)
		for (auto [h, xf, box] : registry.view<TransformComponent, BoxCollider3DComponent>().each())
		{
			if (visited.count(h))
				continue;

			const glm::mat4 world = getWorldMatrix(h, xf);
			const glm::vec3 localMin = box.Offset - box.HalfExtents;
			const glm::vec3 localMax = box.Offset + box.HalfExtents;

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);
			const glm::vec3 ext = (worldMax - worldMin) * 0.5f;
			if (ext.y < 0.6f && (ext.x > 4.0f || ext.z > 4.0f))
				continue;
			if (ext.x < 0.10f && ext.y < 0.10f && ext.z < 0.10f)
				continue;

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		// 7. 3D 球体碰撞体 (SphereCollider3DComponent)
		for (auto [h, xf, sphere] : registry.view<TransformComponent, SphereCollider3DComponent>().each())
		{
			if (visited.count(h))
				continue;

			const glm::mat4 world = getWorldMatrix(h, xf);
			const glm::vec3 localMin = sphere.Offset - glm::vec3(sphere.Radius);
			const glm::vec3 localMax = sphere.Offset + glm::vec3(sphere.Radius);

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		// 8. 3D 胶囊体碰撞体 (CapsuleCollider3DComponent)
		for (auto [h, xf, cap] : registry.view<TransformComponent, CapsuleCollider3DComponent>().each())
		{
			if (visited.count(h))
				continue;

			const glm::mat4 world = getWorldMatrix(h, xf);
			const glm::vec3 localMin = cap.Offset + glm::vec3 { -cap.Radius, -(cap.HalfHeight + cap.Radius), -cap.Radius };
			const glm::vec3 localMax = cap.Offset + glm::vec3 {  cap.Radius,  (cap.HalfHeight + cap.Radius),  cap.Radius };

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		// 9. 动态导航障碍体 (NavMeshObstacleComponent)
		for (auto [h, xf, obs] : registry.view<TransformComponent, NavMeshObstacleComponent>().each())
		{
			if (visited.count(h))
				continue;

			const glm::mat4 world = getWorldMatrix(h, xf);
			const glm::vec3 localMin = -obs.Extents;
			const glm::vec3 localMax =  obs.Extents;

			glm::vec3 worldMin, worldMax;
			TransformAabb(world, localMin, localMax, worldMin, worldMax);
			const glm::vec3 ext = (worldMax - worldMin) * 0.5f;
			if (ext.y < 0.6f && (ext.x > 4.0f || ext.z > 4.0f))
				continue;
			if (ext.x < 0.10f && ext.y < 0.10f && ext.z < 0.10f)
				continue;

			SceneOccluder occ;
			occ.WorldMin = worldMin;
			occ.WorldMax = worldMax;
			occ.LocalMin = localMin;
			occ.LocalMax = localMax;
			const float det = glm::determinant(world);
			if (std::abs(det) > 1e-6f)
			{
				occ.InvWorldMatrix = glm::inverse(world);
				occ.HasInvMatrix = true;
			}
			occ.Owner = h;
			occluders.push_back(occ);
			visited.insert(h);
		}

		return occluders;
	}
}
