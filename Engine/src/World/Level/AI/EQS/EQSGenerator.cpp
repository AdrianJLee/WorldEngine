#include "wldpch.h"
#include "World/Level/AI/EQS/EQSGenerator.h"
#include "World/Terrain/Terrain.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace World::EQS
{
	void EQSGenerator_ProjectedGrid::GenerateItems(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& outItems)
	{
		const float r = (Radius > 0.0f) ? Radius : ctx.QueryRadius;
		float s = (Spacing > 0.05f) ? Spacing : ctx.GridSpacing;
		if (s <= 0.05f)
			s = 1.0f;
		if (r <= 0.0f)
			return;

		const int steps = static_cast<int>(r / s);
		for (int z = -steps; z <= steps; ++z)
		{
			for (int x = -steps; x <= steps; ++x)
			{
				const glm::vec3 pt = ctx.QuerierPos + glm::vec3(x * s, 0.0f, z * s);
				const float distToQuerier = glm::distance(glm::vec2(pt.x, pt.z), glm::vec2(ctx.QuerierPos.x, ctx.QuerierPos.z));
				if (distToQuerier <= r)
				{
					EQSCandidateItem item;
					item.Position = pt;
					item.TotalScore = 0.0f;
					item.FilteredOut = false;
					outItems.push_back(item);
				}
			}
		}

		if (ProjectToTerrain && ctx.Terrain)
		{
			const glm::mat4 invTerrain = glm::inverse(ctx.TerrainWorldTransform);
			for (auto& item : outItems)
			{
				glm::vec3 localPt = glm::vec3(invTerrain * glm::vec4(item.Position, 1.0f));
				float h = ctx.Terrain->SampleHeight(localPt.x, localPt.z);
				glm::vec3 worldGround = glm::vec3(ctx.TerrainWorldTransform * glm::vec4(localPt.x, h, localPt.z, 1.0f));
				item.Position.y = worldGround.y;
			}
		}
	}

	void EQSGenerator_Circle::GenerateItems(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& outItems)
	{
		const float innerR = InnerRadius;
		const float outerR = (OuterRadius > 0.0f) ? OuterRadius : ctx.QueryRadius;
		float step = (StepDistance > 0.05f) ? StepDistance : ctx.GridSpacing;
		if (step <= 0.05f)
			step = 1.0f;
		if (outerR <= 0.0f)
			return;

		if (innerR <= 0.001f)
		{
			EQSCandidateItem center;
			center.Position = ctx.QuerierPos;
			center.TotalScore = 0.0f;
			center.FilteredOut = false;
			outItems.push_back(center);
		}

		const int ringCount = static_cast<int>((outerR - innerR) / step);
		for (int r = 1; r <= ringCount; ++r)
		{
			const float radius = innerR + r * step;
			const float circumference = 2.0f * 3.14159265f * radius;
			const int pointCount = std::max(MinPointsPerRing, static_cast<int>(circumference / step));
			for (int i = 0; i < pointCount; ++i)
			{
				const float angle = (static_cast<float>(i) / static_cast<float>(pointCount)) * 2.0f * 3.14159265f;
				glm::vec3 pt = ctx.QuerierPos + glm::vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
				EQSCandidateItem item;
				item.Position = pt;
				item.TotalScore = 0.0f;
				item.FilteredOut = false;
				outItems.push_back(item);
			}
		}

		if (ProjectToTerrain && ctx.Terrain)
		{
			const glm::mat4 invTerrain = glm::inverse(ctx.TerrainWorldTransform);
			for (auto& item : outItems)
			{
				glm::vec3 localPt = glm::vec3(invTerrain * glm::vec4(item.Position, 1.0f));
				float h = ctx.Terrain->SampleHeight(localPt.x, localPt.z);
				glm::vec3 worldGround = glm::vec3(ctx.TerrainWorldTransform * glm::vec4(localPt.x, h, localPt.z, 1.0f));
				item.Position.y = worldGround.y;
			}
		}
	}

	void EQSGenerator_AroundObstacles::GenerateItems(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& outItems)
	{
		const float r = ctx.QueryRadius;
		float s = (ctx.GridSpacing > 0.05f) ? ctx.GridSpacing : 1.0f;

		if (IncludeGrid && r > 0.0f)
		{
			const int steps = static_cast<int>(r / s);
			for (int z = -steps; z <= steps; ++z)
			{
				for (int x = -steps; x <= steps; ++x)
				{
					const glm::vec3 pt = ctx.QuerierPos + glm::vec3(x * s, 0.0f, z * s);
					const float distToQuerier = glm::distance(glm::vec2(pt.x, pt.z), glm::vec2(ctx.QuerierPos.x, ctx.QuerierPos.z));
					if (distToQuerier <= r)
					{
						EQSCandidateItem item;
						item.Position = pt;
						item.TotalScore = 0.0f;
						item.FilteredOut = false;
						outItems.push_back(item);
					}
				}
			}
		}

		for (const auto& box : ctx.Obstacles)
		{
			const float offset = WallOffset;
			const float ex = box.HalfExtents.x + offset;
			const float ez = box.HalfExtents.z + offset;

			const std::vector<glm::vec3> localOffsets = {
				{ +ex, 0.0f,  0.0f },
				{ -ex, 0.0f,  0.0f },
				{  0.0f, 0.0f, +ez },
				{  0.0f, 0.0f, -ez },
				{ +ex, 0.0f, +ez * 0.5f },
				{ +ex, 0.0f, -ez * 0.5f },
				{ -ex, 0.0f, +ez * 0.5f },
				{ -ex, 0.0f, -ez * 0.5f },
				{ +ex * 0.5f, 0.0f, +ez },
				{ -ex * 0.5f, 0.0f, +ez },
				{ +ex * 0.5f, 0.0f, -ez },
				{ -ex * 0.5f, 0.0f, -ez }
			};

			for (const auto& lOff : localOffsets)
			{
				glm::vec3 worldPt = box.Center + box.Rotation * lOff;
				worldPt.y = ctx.QuerierPos.y;
				const float dist = glm::distance(glm::vec2(worldPt.x, worldPt.z), glm::vec2(ctx.QuerierPos.x, ctx.QuerierPos.z));
				if (dist <= r)
				{
					EQSCandidateItem item;
					item.Position = worldPt;
					item.TotalScore = 0.0f;
					item.FilteredOut = false;
					outItems.push_back(item);
				}
			}
		}

		if (ProjectToTerrain && ctx.Terrain)
		{
			const glm::mat4 invTerrain = glm::inverse(ctx.TerrainWorldTransform);
			for (auto& item : outItems)
			{
				glm::vec3 localPt = glm::vec3(invTerrain * glm::vec4(item.Position, 1.0f));
				float h = ctx.Terrain->SampleHeight(localPt.x, localPt.z);
				glm::vec3 worldGround = glm::vec3(ctx.TerrainWorldTransform * glm::vec4(localPt.x, h, localPt.z, 1.0f));
				item.Position.y = worldGround.y;
			}
		}
	}

	EQSGeneratorRegistry::EQSGeneratorRegistry()
	{
		Register("ProjectedGrid", std::make_shared<EQSGenerator_ProjectedGrid>());
		Register("Circle", std::make_shared<EQSGenerator_Circle>());
		Register("AroundObstacles", std::make_shared<EQSGenerator_AroundObstacles>());
	}

	EQSGeneratorRegistry& EQSGeneratorRegistry::Get()
	{
		static EQSGeneratorRegistry s_Instance;
		return s_Instance;
	}

	void EQSGeneratorRegistry::Register(const std::string& name, std::shared_ptr<IEQSGenerator> generator)
	{
		m_Generators[name] = std::move(generator);
	}

	std::shared_ptr<IEQSGenerator> EQSGeneratorRegistry::Find(const std::string& name) const
	{
		auto it = m_Generators.find(name);
		if (it != m_Generators.end())
			return it->second;
		return nullptr;
	}
}
