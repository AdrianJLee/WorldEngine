#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	struct SplinePoint
	{
		glm::vec3 Position { 0.0f };
		glm::vec3 Tangent { 0.0f, 0.0f, 1.0f };
	};

	class WLD_API Spline3D
	{
	public:
		Spline3D() = default;
		~Spline3D() = default;

		void Clear() { m_Points.clear(); }
		void AddPoint(const glm::vec3& pos);
		size_t GetPointCount() const { return m_Points.size(); }
		const SplinePoint& GetPoint(size_t index) const { return m_Points[index]; }

		// Catmull-Rom 参数采样 (t ∈ [0, 1])
		void SamplePoint(float t, glm::vec3& outPos, glm::vec3& outTangent) const;

		// 弧长累加与等距采样
		float GetTotalLength() const;
		void SampleAtDistance(float distance, glm::vec3& outPos, glm::vec3& outTangent) const;

		// 沿样条线放样生成路面双侧多边形带
		void GenerateRoadMesh(
			float roadWidth,
			float stepDistance,
			std::vector<glm::vec3>& outVerts,
			std::vector<glm::vec3>& outNormals,
			std::vector<glm::vec2>& outUVs,
			std::vector<uint32_t>& outIndices) const;

	private:
		std::vector<SplinePoint> m_Points;
	};
}
