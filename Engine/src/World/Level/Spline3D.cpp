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

		constexpr int kArcSamples = 300;

		void BuildArcLengthLUT(const Spline3D& spline, std::vector<float>& outCumDist, float& outTotalLen)
		{
			outCumDist.resize(kArcSamples + 1);
			outCumDist[0] = 0.0f;
			glm::vec3 prevPos, dummy;
			spline.SamplePoint(0.0f, prevPos, dummy);
			float total = 0.0f;
			for (int i = 1; i <= kArcSamples; ++i)
			{
				const float t = static_cast<float>(i) / static_cast<float>(kArcSamples);
				glm::vec3 curPos;
				spline.SamplePoint(t, curPos, dummy);
				total += glm::distance(curPos, prevPos);
				outCumDist[i] = total;
				prevPos = curPos;
			}
			outTotalLen = total;
		}
	}

	void Spline3D::UpdateTangents()
	{
		const size_t n = m_Points.size();
		if (n < 2)
			return;

		for (size_t i = 0; i < n; ++i)
		{
			glm::vec3 prev, next;
			if (m_ClosedLoop)
			{
				prev = m_Points[(i + n - 1) % n].Position;
				next = m_Points[(i + 1) % n].Position;
			}
			else
			{
				prev = (i > 0) ? m_Points[i - 1].Position : m_Points[i].Position;
				next = (i + 1 < n) ? m_Points[i + 1].Position : m_Points[i].Position;
			}
			const glm::vec3 tan = next - prev;
			m_Points[i].Tangent = (glm::length(tan) > 1e-4f) ? glm::normalize(tan) : glm::vec3(0, 0, 1);
		}
	}

	void Spline3D::AddPoint(const glm::vec3& pos)
	{
		SplinePoint pt;
		pt.Position = pos;
		m_Points.push_back(pt);
		UpdateTangents();
	}

	void Spline3D::SetPoint(size_t index, const glm::vec3& pos)
	{
		if (index < m_Points.size())
		{
			m_Points[index].Position = pos;
			UpdateTangents();
		}
	}

	void Spline3D::RemovePoint(size_t index)
	{
		if (index < m_Points.size())
		{
			m_Points.erase(m_Points.begin() + index);
			UpdateTangents();
		}
	}

	void Spline3D::SetClosedLoop(bool closed)
	{
		m_ClosedLoop = closed;
		UpdateTangents();
	}

	void Spline3D::SetMode(SplineMode mode)
	{
		m_Mode = mode;
		UpdateTangents();
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
		const size_t n = m_Points.size();

		if (m_ClosedLoop)
		{
			const size_t segmentCount = n;
			const float segProgress = clampedT * static_cast<float>(segmentCount);
			const size_t segIdx = std::min(static_cast<size_t>(segProgress), segmentCount - 1);
			const float localT = segProgress - static_cast<float>(segIdx);

			const glm::vec3& p1 = m_Points[segIdx].Position;
			const glm::vec3& p2 = m_Points[(segIdx + 1) % segmentCount].Position;
			const glm::vec3& p0 = m_Points[(segIdx + segmentCount - 1) % segmentCount].Position;
			const glm::vec3& p3 = m_Points[(segIdx + 2) % segmentCount].Position;

			if (m_Mode == SplineMode::Bezier)
			{
				const float segDist = glm::distance(p1, p2);
				const glm::vec3 c1 = p1 + m_Points[segIdx].Tangent * (segDist / 3.0f);
				const glm::vec3 c2 = p2 - m_Points[(segIdx + 1) % segmentCount].Tangent * (segDist / 3.0f);

				const float u = localT;
				const float inv = 1.0f - u;
				outPos = inv * inv * inv * p1 + 3.0f * inv * inv * u * c1 + 3.0f * inv * u * u * c2 + u * u * u * p2;
				const glm::vec3 tan = 3.0f * inv * inv * (c1 - p1) + 6.0f * inv * u * (c2 - c1) + 3.0f * u * u * (p2 - c2);
				outTangent = (glm::length(tan) > 1e-4f) ? glm::normalize(tan) : glm::vec3(0, 0, 1);
			}
			else
			{
				outPos = CatmullRom(p0, p1, p2, p3, localT);
				const glm::vec3 tan = CatmullRomTangent(p0, p1, p2, p3, localT);
				outTangent = (glm::length(tan) > 1e-4f) ? glm::normalize(tan) : glm::vec3(0, 0, 1);
			}
		}
		else
		{
			const size_t segmentCount = n - 1;
			const float segProgress = clampedT * static_cast<float>(segmentCount);
			const size_t segIdx = std::min(static_cast<size_t>(segProgress), segmentCount - 1);
			const float localT = segProgress - static_cast<float>(segIdx);

			const glm::vec3& p1 = m_Points[segIdx].Position;
			const glm::vec3& p2 = m_Points[segIdx + 1].Position;
			const glm::vec3& p0 = (segIdx > 0) ? m_Points[segIdx - 1].Position : (p1 - (p2 - p1));
			const glm::vec3& p3 = (segIdx + 2 < n) ? m_Points[segIdx + 2].Position : (p2 + (p2 - p1));

			if (m_Mode == SplineMode::Bezier)
			{
				const float segDist = glm::distance(p1, p2);
				const glm::vec3 c1 = p1 + m_Points[segIdx].Tangent * (segDist / 3.0f);
				const glm::vec3 c2 = p2 - m_Points[segIdx + 1].Tangent * (segDist / 3.0f);

				const float u = localT;
				const float inv = 1.0f - u;
				outPos = inv * inv * inv * p1 + 3.0f * inv * inv * u * c1 + 3.0f * inv * u * u * c2 + u * u * u * p2;
				const glm::vec3 tan = 3.0f * inv * inv * (c1 - p1) + 6.0f * inv * u * (c2 - c1) + 3.0f * u * u * (p2 - c2);
				outTangent = (glm::length(tan) > 1e-4f) ? glm::normalize(tan) : glm::vec3(0, 0, 1);
			}
			else
			{
				outPos = CatmullRom(p0, p1, p2, p3, localT);
				const glm::vec3 tan = CatmullRomTangent(p0, p1, p2, p3, localT);
				outTangent = (glm::length(tan) > 1e-4f) ? glm::normalize(tan) : glm::vec3(0, 0, 1);
			}
		}
	}

	float Spline3D::GetTotalLength() const
	{
		if (m_Points.size() < 2)
			return 0.0f;

		std::vector<float> lut;
		float totalLen = 0.0f;
		BuildArcLengthLUT(*this, lut, totalLen);
		return totalLen;
	}

	void Spline3D::SampleAtDistance(float distance, glm::vec3& outPos, glm::vec3& outTangent) const
	{
		if (m_Points.size() < 2)
		{
			SamplePoint(0.0f, outPos, outTangent);
			return;
		}

		std::vector<float> lut;
		float totalLen = 0.0f;
		BuildArcLengthLUT(*this, lut, totalLen);

		if (totalLen <= 1e-4f)
		{
			SamplePoint(0.0f, outPos, outTangent);
			return;
		}

		const float d = std::clamp(distance, 0.0f, totalLen);
		auto it = std::lower_bound(lut.begin(), lut.end(), d);
		if (it == lut.begin())
		{
			SamplePoint(0.0f, outPos, outTangent);
			return;
		}
		if (it == lut.end())
		{
			SamplePoint(1.0f, outPos, outTangent);
			return;
		}

		const size_t idx = static_cast<size_t>(std::distance(lut.begin(), it));
		const size_t prevIdx = idx - 1;
		const float segLen = lut[idx] - lut[prevIdx];
		const float alpha = (segLen > 1e-5f) ? (d - lut[prevIdx]) / segLen : 0.0f;
		const float t = (static_cast<float>(prevIdx) + alpha) / static_cast<float>(kArcSamples);

		SamplePoint(t, outPos, outTangent);
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
		if (totalLen <= 0.05f || stepDistance <= 0.01f || roadWidth <= 0.01f)
			return;

		const uint32_t numSteps = std::max(2u, static_cast<uint32_t>(std::ceil(totalLen / stepDistance)));
		const float halfW = roadWidth * 0.5f;

		for (uint32_t i = 0; i <= numSteps; ++i)
		{
			const float d = std::min(static_cast<float>(i) * (totalLen / static_cast<float>(numSteps)), totalLen);
			glm::vec3 pos, tangent;
			SampleAtDistance(d, pos, tangent);

			glm::vec3 up(0.0f, 1.0f, 0.0f);
			if (std::abs(glm::dot(up, tangent)) > 0.99f)
				up = glm::vec3(0.0f, 0.0f, 1.0f);

			glm::vec3 right = glm::cross(up, tangent);
			if (glm::length(right) < 1e-4f)
				right = glm::vec3(1.0f, 0.0f, 0.0f);
			else
				right = glm::normalize(right);

			const glm::vec3 normal = glm::normalize(glm::cross(tangent, right));

			// 左侧与右侧顶点
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

		if (m_ClosedLoop && numSteps >= 2)
		{
			const uint32_t lastBase = numSteps * 2;
			outIndices.push_back(lastBase + 0);
			outIndices.push_back(0);
			outIndices.push_back(lastBase + 1);

			outIndices.push_back(lastBase + 1);
			outIndices.push_back(0);
			outIndices.push_back(1);
		}
	}
}
