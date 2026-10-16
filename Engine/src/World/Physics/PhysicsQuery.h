#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <vector>

namespace World::Physics
{
	struct RaycastHit
	{
		entt::entity Entity = entt::null;
		glm::vec3 Point { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		float Distance = 0.0f;
		bool Hit = false;
	};

	struct ShapeCastHit : public RaycastHit
	{
		float PenetrationDepth = 0.0f;
	};

	class WLD_API PhysicsQuerySystem
	{
	public:
		// 射线检测: 从 origin 出发沿 direction 探测最大 maxDistance
		static RaycastHit Raycast(Scene& scene, const glm::vec3& origin, const glm::vec3& direction,
		                          float maxDistance = 1000.0f, uint32_t layerMask = 0xFFFFFFFF);

		// 2D 射线检测别名: 强制在 XY 平面 (忽略 Z 轴)
		static RaycastHit Raycast2D(Scene& scene, const glm::vec2& origin, const glm::vec2& direction,
		                            float maxDistance = 1000.0f, uint32_t layerMask = 0xFFFFFFFF);

		// 球体 / 圆形扫掠
		static ShapeCastHit SphereCast(Scene& scene, const glm::vec3& origin, float radius,
		                               const glm::vec3& direction, float maxDistance = 1000.0f, uint32_t layerMask = 0xFFFFFFFF);

		// 胶囊体扫掠 (3D 胶囊 / 2D 胶囊)
		static ShapeCastHit CapsuleCast(Scene& scene, const glm::vec3& origin, float radius, float halfHeight,
		                                const glm::vec3& direction, float maxDistance = 1000.0f, uint32_t layerMask = 0xFFFFFFFF);

		// 轴对齐盒体扫掠
		static ShapeCastHit BoxCast(Scene& scene, const glm::vec3& origin, const glm::vec3& halfExtents,
		                            const glm::vec3& direction, float maxDistance = 1000.0f, uint32_t layerMask = 0xFFFFFFFF);

		// 区域重叠检测: 球体 / 圆形
		static std::vector<entt::entity> OverlapSphere(Scene& scene, const glm::vec3& center, float radius,
		                                               uint32_t layerMask = 0xFFFFFFFF);

		// 区域重叠检测: 盒体
		static std::vector<entt::entity> OverlapBox(Scene& scene, const glm::vec3& center, const glm::vec3& halfExtents,
		                                            uint32_t layerMask = 0xFFFFFFFF);
	};
}
