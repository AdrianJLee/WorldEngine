#include "wldpch.h"
#include "World/Level/Core/WorldSpace.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"

namespace World::Level
{
	WorldSpace::WorldSpace(float rebaseThreshold)
		: m_RebaseThreshold(rebaseThreshold > 0.0f ? rebaseThreshold : 1000.0f)
	{
	}

	bool WorldSpace::NeedsRebase(const glm::vec3& cameraLocalPos) const
	{
		return glm::length(cameraLocalPos) >= m_RebaseThreshold;
	}

	void WorldSpace::Rebase(Scene& scene, const glm::vec3& newLocalCenter)
	{
		m_OriginOffset += glm::dvec3(newLocalCenter);

		auto view = scene.GetRegistry().view<TransformComponent>();
		for (auto entity : view)
		{
			auto& tc = view.get<TransformComponent>(entity);
			tc.Location -= newLocalCenter;
		}
	}
}
