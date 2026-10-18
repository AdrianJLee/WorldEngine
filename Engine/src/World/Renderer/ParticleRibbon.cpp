#include "wldpch.h"
#include "World/Renderer/ParticleRibbon.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/Renderer/ParticleBuffer.h"
#include <algorithm>
#include <cmath>

namespace World
{
	ParticleRibbon::ParticleRibbon(const RibbonConfig& config)
		: m_Config(config)
	{
	}

	void ParticleRibbon::AddPoint(
		const glm::vec3& position,
		const glm::vec4& color,
		float width,
		float maxAge)
	{
		if (!m_Points.empty())
		{
			float dist = glm::distance(position, m_Points.front().Position);
			if (dist < m_Config.MinVertexDistance)
			{
				m_Points.front().Position = position;
				m_Points.front().Color = color;
				return;
			}
		}

		RibbonControlPoint pt;
		pt.Position = position;
		pt.Color = color;
		pt.Width = (width > 0.0f) ? width : m_Config.InitialWidth;
		pt.Age = 0.0f;
		pt.MaxAge = (maxAge > 0.0f) ? maxAge : m_Config.MaxAge;

		m_Points.insert(m_Points.begin(), pt);
	}

	void ParticleRibbon::Update(float dt)
	{
		for (auto& pt : m_Points)
		{
			pt.Age += dt;
		}

		while (!m_Points.empty() && m_Points.back().Age >= m_Points.back().MaxAge)
		{
			m_Points.pop_back();
		}
	}

	void ParticleRibbon::Clear()
	{
		m_Points.clear();
	}

	void ParticleRibbon::GenerateRibbonVertices(
		const glm::vec3& cameraPos,
		std::vector<RibbonVertex>& outVertices,
		std::vector<uint32_t>& outIndices) const
	{
		outVertices.clear();
		outIndices.clear();

		const size_t N = m_Points.size();
		if (N < 2)
			return;

		const uint32_t steps = std::max(1u, m_Config.SubdivisionsPerSegment);
		float accumulatedDistance = 0.0f;
		glm::vec3 prevPointPos = m_Points[0].Position;

		uint32_t crossSectionCount = 0;

		for (size_t i = 0; i < N - 1; ++i)
		{
			const auto& p0 = (i == 0) ? m_Points[0] : m_Points[i - 1];
			const auto& p1 = m_Points[i];
			const auto& p2 = m_Points[i + 1];
			const auto& p3 = (i + 2 < N) ? m_Points[i + 2] : m_Points[i + 1];

			uint32_t subSteps = (i == N - 2) ? (steps + 1) : steps;

			for (uint32_t s = 0; s < subSteps; ++s)
			{
				float t = static_cast<float>(s) / static_cast<float>(steps);
				float t2 = t * t;
				float t3 = t2 * t;

				// Catmull-Rom Position
				glm::vec3 pos = 0.5f * (
					(2.0f * p1.Position) +
					(-p0.Position + p2.Position) * t +
					(2.0f * p0.Position - 5.0f * p1.Position + 4.0f * p2.Position - p3.Position) * t2 +
					(-p0.Position + 3.0f * p1.Position - 3.0f * p2.Position + p3.Position) * t3
				);

				// Catmull-Rom Tangent Derivative
				glm::vec3 tangent = 0.5f * (
					(-p0.Position + p2.Position) +
					(4.0f * p0.Position - 10.0f * p1.Position + 8.0f * p2.Position - 2.0f * p3.Position) * t +
					(-3.0f * p0.Position + 9.0f * p1.Position - 9.0f * p2.Position + 3.0f * p3.Position) * t2
				);

				float tangentLen = glm::length(tangent);
				if (tangentLen > 1e-4f)
					tangent /= tangentLen;
				else
					tangent = glm::normalize(p2.Position - p1.Position + glm::vec3(0.0f, 0.001f, 0.0f));

				if (crossSectionCount > 0)
				{
					accumulatedDistance += glm::distance(pos, prevPointPos);
				}
				prevPointPos = pos;

				float V = accumulatedDistance / std::max(0.01f, m_Config.TextureTileLength);

				// Interpolate Age and MaxAge
				float age = glm::mix(p1.Age, p2.Age, t);
				float maxAge = std::max(1e-4f, glm::mix(p1.MaxAge, p2.MaxAge, t));
				float uAge = glm::clamp(age / maxAge, 0.0f, 1.0f);

				// Width and Color
				float baseWidth = glm::mix(p1.Width, p2.Width, t);
				float width = baseWidth * (m_Config.TaperWidthOverLife ? (1.0f - uAge) : 1.0f);
				glm::vec4 color = glm::mix(p1.Color, p2.Color, t);
				if (m_Config.FadeAlphaOverLife)
				{
					color.a *= (1.0f - uAge);
				}

				// Camera-facing Side vector
				glm::vec3 camDir = cameraPos - pos;
				glm::vec3 side = glm::cross(tangent, camDir);
				float sideLen = glm::length(side);
				if (sideLen > 1e-4f)
				{
					side /= sideLen;
				}
				else
				{
					side = glm::cross(tangent, glm::vec3(0.0f, 1.0f, 0.0f));
					if (glm::length(side) < 1e-4f)
						side = glm::cross(tangent, glm::vec3(1.0f, 0.0f, 0.0f));
					side = glm::normalize(side);
				}

				glm::vec3 normal = glm::normalize(glm::cross(side, tangent));
				glm::vec3 halfWidthOffset = side * (width * 0.5f);

				// Emit Left Vertex (U = 0.0)
				RibbonVertex leftVert;
				leftVert.Position = pos - halfWidthOffset;
				leftVert.Normal = normal;
				leftVert.Color = color;
				leftVert.UV = glm::vec2(0.0f, V);
				leftVert.Width = width;
				outVertices.push_back(leftVert);

				// Emit Right Vertex (U = 1.0)
				RibbonVertex rightVert;
				rightVert.Position = pos + halfWidthOffset;
				rightVert.Normal = normal;
				rightVert.Color = color;
				rightVert.UV = glm::vec2(1.0f, V);
				rightVert.Width = width;
				outVertices.push_back(rightVert);

				crossSectionCount++;
			}
		}

		if (crossSectionCount >= 2)
		{
			for (uint32_t k = 0; k < crossSectionCount - 1; ++k)
			{
				uint32_t i0 = k * 2;
				uint32_t i1 = k * 2 + 1;
				uint32_t i2 = (k + 1) * 2;
				uint32_t i3 = (k + 1) * 2 + 1;

				// Triangle 1: (i0, i1, i2)
				outIndices.push_back(i0);
				outIndices.push_back(i1);
				outIndices.push_back(i2);

				// Triangle 2: (i1, i3, i2)
				outIndices.push_back(i1);
				outIndices.push_back(i3);
				outIndices.push_back(i2);
			}
		}
	}

	void ParticleRibbon::GenerateMesh(
		const glm::vec3& cameraPos,
		std::vector<ParticleVertex>& outVertices,
		std::vector<uint32_t>& outIndices) const
	{
		std::vector<RibbonVertex> ribbonVerts;
		GenerateRibbonVertices(cameraPos, ribbonVerts, outIndices);

		outVertices.resize(ribbonVerts.size());
		for (size_t i = 0; i < ribbonVerts.size(); ++i)
		{
			outVertices[i].Position = ribbonVerts[i].Position;
			outVertices[i].Color = ribbonVerts[i].Color;
			outVertices[i].UV = ribbonVerts[i].UV;
			outVertices[i].SoftFadeFactor = 1.0f;
		}
	}
}
