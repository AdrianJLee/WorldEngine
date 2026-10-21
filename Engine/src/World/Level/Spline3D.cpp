#include "wldpch.h"
#include "World/Level/Spline3D.h"

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t)
		{
			const float t2 = t * t;
			const float t3 = t2 * t;
			return 0.5f * ((2.0f * p1) +
				(-p0 + p2) * t +
				(2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
				(-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
		}

		glm::vec3 CatmullRomTangent(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t)
		{
			const float t2 = t * t;
			return 0.5f * ((-p0 + p2) +
				(2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * 2.0f * t +
				(-p0 + 3.0f * p1 - 3.0f * p2 + p3) * 3.0f * t2);
		}
	}

	void Spline3D::AddPoint(const glm::vec3& pos)
	{
		SplinePoint pt;
		pt.Position = pos;
		m_Points.push_back(pt);

		// 自动更新切线
		if (m_Points.size() >= 2)
		{
			for (size_t i = 0; i < m_Points.size(); ++i)
			{
				glm::vec3 prev = (i > 0) ? m_Points[i - 1].Position : m_Points[i].Position;
				glm::vec3 next = (i + 1 < m_Points.size()) ? m_Points[i + 1].Position : m_Points[i].Position;
				const glm::vec3 tan = next - prev;
				m_Points[i].Tangent = (glm::length(tan) > 1e-4f) ? glm::normalize(tan) : glm::vec3(0, 0, 1);
			}
		}
	}

	void Spline3D::SamplePoint(float t, glm::vec3& outPos, glm::vec3& outTangent) const
	{
		if (m_Points.empty())
		{
			outPos = glm::vec3(0.0f);
			outTangent = glm::vec3(0.0f, 0.0f, 1.0f);
			return;
		}
		if (m_Points.size() == 1)
		{
			outPos = m_Points[0].Position;
			outTangent = m_Points[0].Tangent;
			return;
		}

		const float clampedT = std::clamp(t, 0.0f, 1.0f);
		const size_t segmentCount = m_Points.size() - 1;
		const float segProgress = clampedT * static_cast<float>(segmentCount);
		const size_t segIdx = std::min(static_cast<size_t>(segProgress), segmentCount - 1);
		const float localT = segProgress - static_cast<float>(segIdx);

		const glm::vec3& p1 = m_Points[segIdx].Position;
		const glm::vec3& p2 = m_Points[segIdx + 1].Position;
		const glm::vec3& p0 = (segIdx > 0) ? m_Points[segIdx - 1].Position : (p1 - (p2 - p1));
		const glm::vec3& p3 = (segIdx + 2 < m_Points.size()) ? m_Points[segIdx + 2].Position : (p2 + (p2 - p1));

		outPos = CatmullRom(p0, p1, p2, p3, localT);
		const glm::vec3 tan = CatmullRomTangent(p0, p1, p2, p3, localT);
		outTangent = (glm::length(tan) > 1e-4f) ? glm::normalize(tan) : glm::vec3(0, 0, 1);
	}

	float Spline3D::GetTotalLength() const
	{
		if (m_Points.size() < 2)
			return 0.0f;

		float length = 0.0f;
		constexpr int kSamples = 100;
		glm::vec3 prevPos, dummy;
		SamplePoint(0.0f, prevPos, dummy);

		for (int i = 1; i <= kSamples; ++i)
		{
			const float t = static_cast<float>(i) / static_cast<float>(kSamples);
			glm::vec3 curPos;
			SamplePoint(t, curPos, dummy);
			length += glm::distance(curPos, prevPos);
			prevPos = curPos;
		}
		return length;
	}

	void Spline3D::SampleAtDistance(float distance, glm::vec3& outPos, glm::vec3& outTangent) const
	{
		const float totalLen = GetTotalLength();
		if (totalLen <= 1e-4f)
		{
			SamplePoint(0.0f, outPos, outTangent);
			return;
		}
		SamplePoint(distance / totalLen, outPos, outTangent);
	}

	void Spline3D::GenerateRoadMesh(
		float roadWidth,
		float stepDistance,
		std::vector<glm::vec3>& outVerts,
		std::vector<glm::vec3>& outNormals,
		std::vector<glm::vec2>& outUVs,
		std::vector<uint32_t>& outIndices) const
	{
		outVerts.clear();
		outNormals.clear();
		outUVs.clear();
		outIndices.clear();

		const float totalLen = GetTotalLength();
		if (totalLen <= 0.1f || stepDistance <= 0.05f)
			return;

		const uint32_t numSteps = std::max(2u, static_cast<uint32_t>(std::ceil(totalLen / stepDistance)));
		const float halfW = roadWidth * 0.5f;

		for (uint32_t i = 0; i <= numSteps; ++i)
		{
			const float d = std::min(static_cast<float>(i) * stepDistance, totalLen);
			glm::vec3 pos, tangent;
			SampleAtDistance(d, pos, tangent);

			const glm::vec3 up(0.0f, 1.0f, 0.0f);
			glm::vec3 right = glm::cross(up, tangent);
			if (glm::length(right) < 1e-4f)
				right = glm::vec3(1.0f, 0.0f, 0.0f);
			else
				right = glm::normalize(right);

			const glm::vec3 normal = glm::normalize(glm::cross(tangent, right));

			// 左点与右点
			outVerts.push_back(pos - right * halfW);
			outVerts.push_back(pos + right * halfW);

			outNormals.push_back(normal);
			outNormals.push_back(normal);

			const float v = d / roadWidth;
			outUVs.push_back(glm::vec2(0.0f, v));
			outUVs.push_back(glm::vec2(1.0f, v));

			if (i > 0)
			{
				const uint32_t base = (i - 1) * 2;
				outIndices.push_back(base + 0);
				outIndices.push_back(base + 2);
				outIndices.push_back(base + 1);

				outIndices.push_back(base + 1);
				outIndices.push_back(base + 2);
				outIndices.push_back(base + 3);
			}
		}
	}
}
