#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace World
{
	struct ParticleVertex;

	struct RibbonControlPoint
	{
		glm::vec3 Position { 0.0f };
		glm::vec4 Color { 1.0f };
		float Width = 0.5f;
		float Age = 0.0f;
		float MaxAge = 1.0f;
	};

	struct RibbonVertex
	{
		glm::vec3 Position { 0.0f };
		glm::vec3 Normal { 0.0f, 0.0f, 1.0f };
		glm::vec4 Color { 1.0f };
		glm::vec2 UV { 0.0f };
		float Width = 0.5f;
	};

	struct RibbonConfig
	{
		float InitialWidth = 0.5f;
		float MaxAge = 1.0f;
		float TextureTileLength = 2.0f;           // World units per 1.0 V UV repetition
		uint32_t SubdivisionsPerSegment = 4;       // Catmull-Rom spline evaluation steps
		bool TaperWidthOverLife = true;
		bool FadeAlphaOverLife = true;
		float MinVertexDistance = 0.005f;          // Avoid zero-length segment degeneracies
	};

	class WLD_API ParticleRibbon
	{
	public:
		explicit ParticleRibbon(const RibbonConfig& config = RibbonConfig {});
		~ParticleRibbon() = default;

		void SetConfig(const RibbonConfig& config) { m_Config = config; }
		const RibbonConfig& GetConfig() const { return m_Config; }
		RibbonConfig& GetConfigMut() { return m_Config; }

		// Adds a new point at the head of the ribbon
		void AddPoint(
			const glm::vec3& position,
			const glm::vec4& color = glm::vec4(1.0f),
			float width = 0.5f,
			float maxAge = 1.0f);

		// Updates lifetime of points and prunes expired trail tail
		void Update(float dt);

		// Resets all points
		void Clear();

		size_t GetControlPointCount() const { return m_Points.size(); }
		const std::vector<RibbonControlPoint>& GetControlPoints() const { return m_Points; }

		// Generates Catmull-Rom smooth triangle strip converted into ParticleVertex stream
		void GenerateMesh(
			const glm::vec3& cameraPos,
			std::vector<ParticleVertex>& outVertices,
			std::vector<uint32_t>& outIndices) const;

		// Generates detailed RibbonVertex stream (with explicit normals and widths)
		void GenerateRibbonVertices(
			const glm::vec3& cameraPos,
			std::vector<RibbonVertex>& outVertices,
			std::vector<uint32_t>& outIndices) const;

	private:
		RibbonConfig m_Config;
		std::vector<RibbonControlPoint> m_Points;
	};
}
