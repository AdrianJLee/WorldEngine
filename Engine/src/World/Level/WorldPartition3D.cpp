#include "wldpch.h"
#include "World/Level/WorldPartition3D.h"
#include <algorithm>
#include <cmath>

namespace World
{
	WorldPartition3D::WorldPartition3D(float cellSize, float streamingRadius)
		: m_CellSize(std::max(1.0f, cellSize)), m_StreamingRadius(std::max(cellSize, streamingRadius))
	{
	}

	PartitionCell3D WorldPartition3D::WorldToCell(const glm::dvec3& pos) const
	{
		return PartitionCell3D {
			static_cast<int32_t>(std::floor(pos.x / static_cast<double>(m_CellSize))),
			static_cast<int32_t>(std::floor(pos.y / static_cast<double>(m_CellSize))),
			static_cast<int32_t>(std::floor(pos.z / static_cast<double>(m_CellSize)))
		};
	}

	void WorldPartition3D::UpdateViewer(
		const glm::dvec3& viewerPos,
		std::vector<PartitionCell3D>& outCellsToLoad,
		std::vector<PartitionCell3D>& outCellsToUnload)
	{
		outCellsToLoad.clear();
		outCellsToUnload.clear();

		const PartitionCell3D centerCell = WorldToCell(viewerPos);
		const int32_t radiusCells = static_cast<int32_t>(std::ceil(m_StreamingRadius / m_CellSize));
		const double radius2 = static_cast<double>(m_StreamingRadius * m_StreamingRadius);

		std::unordered_set<PartitionCell3D> newDesired;

		for (int32_t dz = -radiusCells; dz <= radiusCells; ++dz)
		{
			for (int32_t dy = -radiusCells; dy <= radiusCells; ++dy)
			{
				for (int32_t dx = -radiusCells; dx <= radiusCells; ++dx)
				{
					const PartitionCell3D candidate { centerCell.X + dx, centerCell.Y + dy, centerCell.Z + dz };
					const glm::dvec3 cellCenter(
						(static_cast<double>(candidate.X) + 0.5) * static_cast<double>(m_CellSize),
						(static_cast<double>(candidate.Y) + 0.5) * static_cast<double>(m_CellSize),
						(static_cast<double>(candidate.Z) + 0.5) * static_cast<double>(m_CellSize)
					);
					const glm::dvec3 delta = cellCenter - viewerPos;
					if (glm::dot(delta, delta) <= radius2)
					{
						newDesired.insert(candidate);
						if (m_ActiveCells.find(candidate) == m_ActiveCells.end())
						{
							outCellsToLoad.push_back(candidate);
						}
					}
				}
			}
		}

		for (const auto& active : m_ActiveCells)
		{
			if (newDesired.find(active) == newDesired.end())
			{
				outCellsToUnload.push_back(active);
			}
		}

		m_ActiveCells = std::move(newDesired);
	}

	void WorldPartition3D::Clear()
	{
		m_ActiveCells.clear();
	}
}
