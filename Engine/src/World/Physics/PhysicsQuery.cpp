#include "wldpch.h"
#include "World/Physics/PhysicsQuery.h"
#include "World/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace World::Physics
{
	namespace
	{
		uint32_t GetEntityLayer(const entt::registry& reg, entt::entity e)
		{
			if (const auto* rb3d = reg.try_get<RigidBody3DComponent>(e))
				return rb3d->Layer;
			if (const auto* rb2d = reg.try_get<RigidBody2DComponent>(e))
				return rb2d->Layer;
			if (const auto* kcc = reg.try_get<CharacterControllerComponent>(e))
				return kcc->CollisionLayer;
			return 1u;
		}

		void GetEntityTransform(const entt::registry& reg, entt::entity e, glm::vec3& outLoc, glm::quat& outRot, glm::vec3& outScale)
		{
			if (const auto* tr = reg.try_get<TransformComponent>(e))
			{
				outLoc = tr->Location;
				outRot = tr->Rotation;
				outScale = tr->Scale;
			}
			else
			{
				outLoc = glm::vec3(0.0f);
				outRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
				outScale = glm::vec3(1.0f);
			}
		}

		float DistancePointToBox(const glm::vec3& p, const glm::vec3& extents, glm::vec3& outClosestPoint, glm::vec3& outNormal)
		{
			glm::vec3 q = glm::abs(p) - extents;
			glm::vec3 d = glm::max(q, glm::vec3(0.0f));
			float outsideDist = glm::length(d);
			if (q.x <= 0.0f && q.y <= 0.0f && q.z <= 0.0f)
			{
				float maxVal = q.x;
				int axis = 0;
				if (q.y > maxVal) { maxVal = q.y; axis = 1; }
				if (q.z > maxVal) { maxVal = q.z; axis = 2; }
				outNormal = glm::vec3(0.0f);
				outNormal[axis] = (p[axis] >= 0.0f) ? 1.0f : -1.0f;
				outClosestPoint = p;
				outClosestPoint[axis] = (p[axis] >= 0.0f) ? extents[axis] : -extents[axis];
				return maxVal;
			}
			outClosestPoint = glm::clamp(p, -extents, extents);
			glm::vec3 diff = p - outClosestPoint;
			float diffLen = glm::length(diff);
			outNormal = (diffLen > 1e-6f) ? (diff / diffLen) : glm::vec3(0.0f, 1.0f, 0.0f);
			return outsideDist;
		}

		bool IntersectRaySphere(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& center, float radius,
		                        float maxDist, float& outDist, glm::vec3& outNormal, glm::vec3& outPoint)
		{
			glm::vec3 m = ro - center;
			float b = glm::dot(m, rd);
			float c = glm::dot(m, m) - radius * radius;
			if (c > 0.0f && b > 0.0f) return false;
			float disc = b * b - c;
			if (disc < 0.0f) return false;
			float t = -b - std::sqrt(disc);
			if (t < 0.0f) t = 0.0f;
			if (t > maxDist) return false;
			outDist = t;
			outPoint = ro + rd * t;
			glm::vec3 n = outPoint - center;
			float nLen = glm::length(n);
			outNormal = (nLen > 1e-6f) ? (n / nLen) : glm::vec3(0.0f, 1.0f, 0.0f);
			return true;
		}

		bool IntersectRayOBB(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& center, const glm::vec3& extents,
		                     const glm::quat& rot, float maxDist, float& outDist, glm::vec3& outNormal, glm::vec3& outPoint)
		{
			glm::quat invRot = glm::conjugate(rot);
			glm::vec3 lro = invRot * (ro - center);
			glm::vec3 lrd = invRot * rd;

			float tmin = 0.0f;
			float tmax = maxDist;
			glm::vec3 hitNormal(0.0f);

			for (int i = 0; i < 3; ++i)
			{
				if (std::abs(lrd[i]) < 1e-8f)
				{
					if (lro[i] < -extents[i] || lro[i] > extents[i]) return false;
				}
				else
				{
					float invD = 1.0f / lrd[i];
					float t1 = (-extents[i] - lro[i]) * invD;
					float t2 = ( extents[i] - lro[i]) * invD;
					glm::vec3 n(0.0f);
					n[i] = -1.0f;
					if (t1 > t2) { std::swap(t1, t2); n[i] = 1.0f; }
					if (t1 > tmin) { tmin = t1; hitNormal = n; }
					tmax = std::min(tmax, t2);
					if (tmin > tmax) return false;
				}
			}
			if (tmin < 0.0f) tmin = 0.0f;
			if (tmin > maxDist) return false;
			outDist = tmin;
			outPoint = ro + rd * tmin;
			outNormal = glm::normalize(rot * hitNormal);
			return true;
		}

		bool IntersectRayCapsule(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& center, float radius, float halfHeight,
		                         const glm::quat& rot, float maxDist, float& outDist, glm::vec3& outNormal, glm::vec3& outPoint)
		{
			glm::quat invRot = glm::conjugate(rot);
			glm::vec3 lro = invRot * (ro - center);
			glm::vec3 lrd = invRot * rd;

			float closestT = maxDist + 1.0f;
			glm::vec3 localNormal(0.0f, 1.0f, 0.0f);
			bool hit = false;

			float a = lrd.x * lrd.x + lrd.z * lrd.z;
			float b = lro.x * lrd.x + lro.z * lrd.z;
			float c = lro.x * lro.x + lro.z * lro.z - radius * radius;

			if (a > 1e-8f)
			{
				float disc = b * b - a * c;
				if (disc >= 0.0f)
				{
					float sq = std::sqrt(disc);
					float t1 = (-b - sq) / a;
					if (t1 >= 0.0f && t1 <= maxDist)
					{
						float y = lro.y + lrd.y * t1;
						if (y >= -halfHeight && y <= halfHeight)
						{
							closestT = t1;
							localNormal = glm::vec3(lro.x + lrd.x * t1, 0.0f, lro.z + lrd.z * t1) / radius;
							hit = true;
						}
					}
				}
			}

			float tBot; glm::vec3 nBot, pBot;
			if (IntersectRaySphere(lro, lrd, glm::vec3(0.0f, -halfHeight, 0.0f), radius, maxDist, tBot, nBot, pBot))
			{
				if (tBot < closestT && (lro.y + lrd.y * tBot) <= -halfHeight)
				{
					closestT = tBot;
					localNormal = nBot;
					hit = true;
				}
			}

			float tTop; glm::vec3 nTop, pTop;
			if (IntersectRaySphere(lro, lrd, glm::vec3(0.0f, halfHeight, 0.0f), radius, maxDist, tTop, nTop, pTop))
			{
				if (tTop < closestT && (lro.y + lrd.y * tTop) >= halfHeight)
				{
					closestT = tTop;
					localNormal = nTop;
					hit = true;
				}
			}

			if (hit && closestT <= maxDist)
			{
				outDist = closestT;
				outPoint = ro + rd * closestT;
				outNormal = glm::normalize(rot * localNormal);
				return true;
			}
			return false;
		}

		bool IntersectRayRoundedBox(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& center, const glm::vec3& extents,
		                            const glm::quat& rot, float roundRadius, float maxDist, float& outDist, glm::vec3& outNormal, glm::vec3& outPoint)
		{
			glm::quat invRot = glm::conjugate(rot);
			glm::vec3 lro = invRot * (ro - center);
			glm::vec3 lrd = invRot * rd;

			glm::vec3 expExtents = extents + glm::vec3(roundRadius);
			float tEntry = 0.0f;
			glm::vec3 dummyN, dummyP;
			if (!IntersectRayOBB(lro, lrd, glm::vec3(0.0f), expExtents, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), maxDist, tEntry, dummyN, dummyP))
				return false;

			float t = tEntry;
			for (int iter = 0; iter < 16; ++iter)
			{
				if (t > maxDist) return false;
				glm::vec3 p = lro + lrd * t;
				glm::vec3 closestP, localN;
				float d = DistancePointToBox(p, extents, closestP, localN) - roundRadius;
				if (d <= 1e-4f)
				{
					outDist = std::max(0.0f, t);
					outPoint = ro + rd * outDist;
					outNormal = glm::normalize(rot * localN);
					return true;
				}
				t += std::max(d, 0.001f);
			}
			return false;
		}

		enum class ColliderType { Box, Sphere, Capsule };

		struct ColliderInfo
		{
			entt::entity Entity = entt::null;
			ColliderType Type = ColliderType::Box;
			uint32_t Layer = 1;
			glm::vec3 Center { 0.0f };
			glm::quat Orientation { 1.0f, 0.0f, 0.0f, 0.0f };
			glm::vec3 HalfExtents { 0.5f };
			float Radius = 0.5f;
			float HalfHeight = 0.5f;
		};

		std::vector<ColliderInfo> CollectColliders(Scene& scene, uint32_t layerMask)
		{
			std::vector<ColliderInfo> colliders;
			auto& reg = scene.GetRegistry();

			for (auto entity : reg.view<BoxCollider3DComponent>())
			{
				uint32_t layer = GetEntityLayer(reg, entity);
				if ((layer & layerMask) == 0) continue;
				const auto& box = reg.get<BoxCollider3DComponent>(entity);
				glm::vec3 loc, scale; glm::quat rot;
				GetEntityTransform(reg, entity, loc, rot, scale);

				ColliderInfo info;
				info.Entity = entity;
				info.Type = ColliderType::Box;
				info.Layer = layer;
				info.Center = loc + rot * box.Offset;
				info.Orientation = rot;
				info.HalfExtents = glm::abs(box.HalfExtents) * glm::abs(scale);
				colliders.push_back(info);
			}

			for (auto entity : reg.view<SphereCollider3DComponent>())
			{
				uint32_t layer = GetEntityLayer(reg, entity);
				if ((layer & layerMask) == 0) continue;
				const auto& sphere = reg.get<SphereCollider3DComponent>(entity);
				glm::vec3 loc, scale; glm::quat rot;
				GetEntityTransform(reg, entity, loc, rot, scale);

				ColliderInfo info;
				info.Entity = entity;
				info.Type = ColliderType::Sphere;
				info.Layer = layer;
				info.Center = loc + rot * sphere.Offset;
				info.Orientation = rot;
				info.Radius = sphere.Radius * std::max({ std::abs(scale.x), std::abs(scale.y), std::abs(scale.z) });
				colliders.push_back(info);
			}

			for (auto entity : reg.view<CapsuleCollider3DComponent>())
			{
				uint32_t layer = GetEntityLayer(reg, entity);
				if ((layer & layerMask) == 0) continue;
				const auto& cap = reg.get<CapsuleCollider3DComponent>(entity);
				glm::vec3 loc, scale; glm::quat rot;
				GetEntityTransform(reg, entity, loc, rot, scale);

				ColliderInfo info;
				info.Entity = entity;
				info.Type = ColliderType::Capsule;
				info.Layer = layer;
				info.Center = loc + rot * cap.Offset;
				info.Orientation = rot;
				info.Radius = cap.Radius * std::max(std::abs(scale.x), std::abs(scale.z));
				info.HalfHeight = cap.HalfHeight * std::abs(scale.y);
				colliders.push_back(info);
			}

			for (auto entity : reg.view<BoxCollider2DComponent>())
			{
				uint32_t layer = GetEntityLayer(reg, entity);
				if ((layer & layerMask) == 0) continue;
				const auto& box2d = reg.get<BoxCollider2DComponent>(entity);
				glm::vec3 loc, scale; glm::quat rot;
				GetEntityTransform(reg, entity, loc, rot, scale);

				ColliderInfo info;
				info.Entity = entity;
				info.Type = ColliderType::Box;
				info.Layer = layer;
				info.Center = glm::vec3(loc.x + box2d.Offset.x, loc.y + box2d.Offset.y, loc.z);
				info.Orientation = rot;
				info.HalfExtents = glm::vec3(box2d.Size.x * std::abs(scale.x), box2d.Size.y * std::abs(scale.y), 1.0f);
				colliders.push_back(info);
			}

			for (auto entity : reg.view<CircleCollider2DComponent>())
			{
				uint32_t layer = GetEntityLayer(reg, entity);
				if ((layer & layerMask) == 0) continue;
				const auto& circle2d = reg.get<CircleCollider2DComponent>(entity);
				glm::vec3 loc, scale; glm::quat rot;
				GetEntityTransform(reg, entity, loc, rot, scale);

				ColliderInfo info;
				info.Entity = entity;
				info.Type = ColliderType::Sphere;
				info.Layer = layer;
				info.Center = glm::vec3(loc.x + circle2d.Offset.x, loc.y + circle2d.Offset.y, loc.z);
				info.Orientation = rot;
				info.Radius = circle2d.Radius * std::max(std::abs(scale.x), std::abs(scale.y));
				colliders.push_back(info);
			}

			for (auto entity : reg.view<CapsuleCollider2DComponent>())
			{
				uint32_t layer = GetEntityLayer(reg, entity);
				if ((layer & layerMask) == 0) continue;
				const auto& cap2d = reg.get<CapsuleCollider2DComponent>(entity);
				glm::vec3 loc, scale; glm::quat rot;
				GetEntityTransform(reg, entity, loc, rot, scale);

				ColliderInfo info;
				info.Entity = entity;
				info.Type = ColliderType::Capsule;
				info.Layer = layer;
				info.Center = glm::vec3(loc.x + cap2d.Offset.x, loc.y + cap2d.Offset.y, loc.z);
				if (cap2d.Direction == 0)
				{
					info.Orientation = rot;
					info.HalfHeight = cap2d.Length * 0.5f * std::abs(scale.y);
					info.Radius = cap2d.Radius * std::abs(scale.x);
				}
				else
				{
					info.Orientation = rot * glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
					info.HalfHeight = cap2d.Length * 0.5f * std::abs(scale.x);
					info.Radius = cap2d.Radius * std::abs(scale.y);
				}
				colliders.push_back(info);
			}

			return colliders;
		}
	}

	RaycastHit PhysicsQuerySystem::Raycast(Scene& scene, const glm::vec3& origin, const glm::vec3& direction,
	                                      float maxDistance, uint32_t layerMask)
	{
		float dirLen = glm::length(direction);
		if (dirLen < 1e-6f || maxDistance <= 0.0f)
			return RaycastHit {};

		glm::vec3 normDir = direction / dirLen;
		auto colliders = CollectColliders(scene, layerMask);

		RaycastHit bestHit;
		bestHit.Distance = maxDistance + 1.0f;

		for (const auto& col : colliders)
		{
			float dist = 0.0f;
			glm::vec3 normal(0.0f, 1.0f, 0.0f);
			glm::vec3 hitPoint(0.0f);
			bool hit = false;

			switch (col.Type)
			{
			case ColliderType::Box:
				hit = IntersectRayOBB(origin, normDir, col.Center, col.HalfExtents, col.Orientation, maxDistance, dist, normal, hitPoint);
				break;
			case ColliderType::Sphere:
				hit = IntersectRaySphere(origin, normDir, col.Center, col.Radius, maxDistance, dist, normal, hitPoint);
				break;
			case ColliderType::Capsule:
				hit = IntersectRayCapsule(origin, normDir, col.Center, col.Radius, col.HalfHeight, col.Orientation, maxDistance, dist, normal, hitPoint);
				break;
			}

			if (hit && dist < bestHit.Distance)
			{
				bestHit.Entity = col.Entity;
				bestHit.Point = hitPoint;
				bestHit.Normal = normal;
				bestHit.Distance = dist;
				bestHit.Hit = true;
			}
		}

		if (!bestHit.Hit)
		{
			bestHit.Distance = 0.0f;
			bestHit.Entity = entt::null;
		}
		return bestHit;
	}

	RaycastHit PhysicsQuerySystem::Raycast2D(Scene& scene, const glm::vec2& origin, const glm::vec2& direction,
	                                        float maxDistance, uint32_t layerMask)
	{
		glm::vec3 o3(origin.x, origin.y, 0.0f);
		glm::vec3 d3(direction.x, direction.y, 0.0f);
		return Raycast(scene, o3, d3, maxDistance, layerMask);
	}

	ShapeCastHit PhysicsQuerySystem::SphereCast(Scene& scene, const glm::vec3& origin, float radius,
	                                           const glm::vec3& direction, float maxDistance, uint32_t layerMask)
	{
		float dirLen = glm::length(direction);
		if (dirLen < 1e-6f || maxDistance <= 0.0f)
			return ShapeCastHit {};

		glm::vec3 normDir = direction / dirLen;
		auto colliders = CollectColliders(scene, layerMask);

		ShapeCastHit bestHit;
		bestHit.Distance = maxDistance + 1.0f;

		for (const auto& col : colliders)
		{
			float dist = 0.0f;
			glm::vec3 normal(0.0f, 1.0f, 0.0f);
			glm::vec3 hitPoint(0.0f);
			bool hit = false;
			float penetration = 0.0f;

			switch (col.Type)
			{
			case ColliderType::Box:
			{
				hit = IntersectRayRoundedBox(origin, normDir, col.Center, col.HalfExtents, col.Orientation, radius, maxDistance, dist, normal, hitPoint);
				if (!hit)
				{
					// Check initial overlap at origin
					glm::quat invRot = glm::conjugate(col.Orientation);
					glm::vec3 lp = invRot * (origin - col.Center);
					glm::vec3 cp, n;
					float d = DistancePointToBox(lp, col.HalfExtents, cp, n);
					if (d < radius)
					{
						hit = true;
						dist = 0.0f;
						penetration = radius - d;
						normal = glm::normalize(col.Orientation * n);
						hitPoint = origin - normal * d;
					}
				}
				break;
			}
			case ColliderType::Sphere:
			{
				float totalRadius = col.Radius + radius;
				hit = IntersectRaySphere(origin, normDir, col.Center, totalRadius, maxDistance, dist, normal, hitPoint);
				if (!hit)
				{
					float d = glm::length(origin - col.Center);
					if (d < totalRadius)
					{
						hit = true;
						dist = 0.0f;
						penetration = totalRadius - d;
						normal = (d > 1e-6f) ? ((origin - col.Center) / d) : glm::vec3(0.0f, 1.0f, 0.0f);
						hitPoint = col.Center + normal * col.Radius;
					}
				}
				break;
			}
			case ColliderType::Capsule:
			{
				float totalRadius = col.Radius + radius;
				hit = IntersectRayCapsule(origin, normDir, col.Center, totalRadius, col.HalfHeight, col.Orientation, maxDistance, dist, normal, hitPoint);
				break;
			}
			}

			if (hit && dist < bestHit.Distance)
			{
				bestHit.Entity = col.Entity;
				bestHit.Point = hitPoint;
				bestHit.Normal = normal;
				bestHit.Distance = dist;
				bestHit.PenetrationDepth = penetration;
				bestHit.Hit = true;
			}
		}

		if (!bestHit.Hit)
		{
			bestHit.Distance = 0.0f;
			bestHit.Entity = entt::null;
		}
		return bestHit;
	}

	ShapeCastHit PhysicsQuerySystem::CapsuleCast(Scene& scene, const glm::vec3& origin, float radius, float halfHeight,
	                                            const glm::vec3& direction, float maxDistance, uint32_t layerMask)
	{
		// A capsule sweep is represented by swept spheres along the capsule spine
		ShapeCastHit hitCenter = SphereCast(scene, origin, radius, direction, maxDistance, layerMask);
		ShapeCastHit hitBottom = SphereCast(scene, origin - glm::vec3(0.0f, halfHeight, 0.0f), radius, direction, maxDistance, layerMask);
		ShapeCastHit hitTop    = SphereCast(scene, origin + glm::vec3(0.0f, halfHeight, 0.0f), radius, direction, maxDistance, layerMask);

		ShapeCastHit bestHit;
		bestHit.Distance = maxDistance + 1.0f;

		const ShapeCastHit* candidates[] = { &hitCenter, &hitBottom, &hitTop };
		for (const auto* c : candidates)
		{
			if (c->Hit && c->Distance < bestHit.Distance)
			{
				bestHit = *c;
			}
		}

		if (!bestHit.Hit)
		{
			bestHit.Distance = 0.0f;
			bestHit.Entity = entt::null;
		}
		return bestHit;
	}

	ShapeCastHit PhysicsQuerySystem::BoxCast(Scene& scene, const glm::vec3& origin, const glm::vec3& halfExtents,
	                                        const glm::vec3& direction, float maxDistance, uint32_t layerMask)
	{
		float dirLen = glm::length(direction);
		if (dirLen < 1e-6f || maxDistance <= 0.0f)
			return ShapeCastHit {};

		glm::vec3 normDir = direction / dirLen;
		auto colliders = CollectColliders(scene, layerMask);

		ShapeCastHit bestHit;
		bestHit.Distance = maxDistance + 1.0f;

		for (const auto& col : colliders)
		{
			float dist = 0.0f;
			glm::vec3 normal(0.0f, 1.0f, 0.0f);
			glm::vec3 hitPoint(0.0f);
			bool hit = false;
			float penetration = 0.0f;

			switch (col.Type)
			{
			case ColliderType::Box:
			{
				glm::vec3 combinedExtents = col.HalfExtents + halfExtents;
				hit = IntersectRayOBB(origin, normDir, col.Center, combinedExtents, col.Orientation, maxDistance, dist, normal, hitPoint);
				break;
			}
			case ColliderType::Sphere:
			{
				hit = IntersectRayRoundedBox(col.Center, -normDir, origin, halfExtents, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), col.Radius, maxDistance, dist, normal, hitPoint);
				if (hit) normal = -normal;
				break;
			}
			case ColliderType::Capsule:
			{
				float r = col.Radius;
				glm::vec3 top = col.Center + glm::vec3(0.0f, col.HalfHeight, 0.0f);
				glm::vec3 bot = col.Center - glm::vec3(0.0f, col.HalfHeight, 0.0f);
				float d1, d2, d3; glm::vec3 n1, n2, n3, p1, p2, p3;
				bool h1 = IntersectRayRoundedBox(col.Center, -normDir, origin, halfExtents, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), r, maxDistance, d1, n1, p1);
				bool h2 = IntersectRayRoundedBox(top, -normDir, origin, halfExtents, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), r, maxDistance, d2, n2, p2);
				bool h3 = IntersectRayRoundedBox(bot, -normDir, origin, halfExtents, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), r, maxDistance, d3, n3, p3);

				float minD = maxDistance + 1.0f;
				if (h1 && d1 < minD) { minD = d1; normal = -n1; hitPoint = p1; hit = true; }
				if (h2 && d2 < minD) { minD = d2; normal = -n2; hitPoint = p2; hit = true; }
				if (h3 && d3 < minD) { minD = d3; normal = -n3; hitPoint = p3; hit = true; }
				if (hit) dist = minD;
				break;
			}
			}

			if (hit && dist < bestHit.Distance)
			{
				bestHit.Entity = col.Entity;
				bestHit.Point = hitPoint;
				bestHit.Normal = normal;
				bestHit.Distance = dist;
				bestHit.PenetrationDepth = penetration;
				bestHit.Hit = true;
			}
		}

		if (!bestHit.Hit)
		{
			bestHit.Distance = 0.0f;
			bestHit.Entity = entt::null;
		}
		return bestHit;
	}

	std::vector<entt::entity> PhysicsQuerySystem::OverlapSphere(Scene& scene, const glm::vec3& center, float radius,
	                                                           uint32_t layerMask)
	{
		std::vector<entt::entity> results;
		auto colliders = CollectColliders(scene, layerMask);

		for (const auto& col : colliders)
		{
			bool overlaps = false;
			switch (col.Type)
			{
			case ColliderType::Sphere:
			{
				float dist = glm::length(center - col.Center);
				overlaps = (dist <= radius + col.Radius);
				break;
			}
			case ColliderType::Box:
			{
				glm::quat invRot = glm::conjugate(col.Orientation);
				glm::vec3 lp = invRot * (center - col.Center);
				glm::vec3 cp, n;
				float d = DistancePointToBox(lp, col.HalfExtents, cp, n);
				overlaps = (d <= radius);
				break;
			}
			case ColliderType::Capsule:
			{
				glm::quat invRot = glm::conjugate(col.Orientation);
				glm::vec3 lp = invRot * (center - col.Center);
				float clampedY = glm::clamp(lp.y, -col.HalfHeight, col.HalfHeight);
				glm::vec3 segPt(0.0f, clampedY, 0.0f);
				float d = glm::length(lp - segPt);
				overlaps = (d <= radius + col.Radius);
				break;
			}
			}

			if (overlaps)
			{
				results.push_back(col.Entity);
			}
		}
		return results;
	}

	std::vector<entt::entity> PhysicsQuerySystem::OverlapBox(Scene& scene, const glm::vec3& center, const glm::vec3& halfExtents,
	                                                        uint32_t layerMask)
	{
		std::vector<entt::entity> results;
		auto colliders = CollectColliders(scene, layerMask);

		for (const auto& col : colliders)
		{
			bool overlaps = false;
			switch (col.Type)
			{
			case ColliderType::Sphere:
			{
				glm::vec3 cp, n;
				float d = DistancePointToBox(col.Center - center, halfExtents, cp, n);
				overlaps = (d <= col.Radius);
				break;
			}
			case ColliderType::Box:
			{
				glm::vec3 diff = glm::abs(center - col.Center);
				glm::vec3 combinedExtents = halfExtents + col.HalfExtents;
				overlaps = (diff.x <= combinedExtents.x && diff.y <= combinedExtents.y && diff.z <= combinedExtents.z);
				break;
			}
			case ColliderType::Capsule:
			{
				glm::vec3 cp, n;
				float d1 = DistancePointToBox(col.Center - center, halfExtents, cp, n);
				float d2 = DistancePointToBox(col.Center + glm::vec3(0.0f, col.HalfHeight, 0.0f) - center, halfExtents, cp, n);
				float d3 = DistancePointToBox(col.Center - glm::vec3(0.0f, col.HalfHeight, 0.0f) - center, halfExtents, cp, n);
				overlaps = (std::min({ d1, d2, d3 }) <= col.Radius);
				break;
			}
			}

			if (overlaps)
			{
				results.push_back(col.Entity);
			}
		}
		return results;
	}
}
