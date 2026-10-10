#pragma once

#include "World/Core/Export.h"

#include <glm/glm.hpp>

namespace World
{
	class Scene;
}

namespace World::Level
{
	class WLD_API WorldSpace
	{
	public:
		WorldSpace(float rebaseThreshold = 1000.0f);
		~WorldSpace() = default;

		const glm::dvec3& GetOriginOffset() const { return m_OriginOffset; }
		void SetOriginOffset(const glm::dvec3& offset) { m_OriginOffset = offset; }

		float GetRebaseThreshold() const { return m_RebaseThreshold; }
		void SetRebaseThreshold(float t) { m_RebaseThreshold = t; }

		glm::dvec3 ToWorldCoordinates(const glm::vec3& localPos) const
		{
			return m_OriginOffset + glm::dvec3(localPos);
		}

		glm::vec3 ToLocalCoordinates(const glm::dvec3& worldPos) const
		{
			return glm::vec3(worldPos - m_OriginOffset);
		}

		bool NeedsRebase(const glm::vec3& cameraLocalPos) const;
		void Rebase(Scene& scene, const glm::vec3& newLocalCenter);

	private:
		glm::dvec3 m_OriginOffset{ 0.0 };
		float m_RebaseThreshold{ 1000.0f };
	};
}
