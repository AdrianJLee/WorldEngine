#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace World
{
	struct PartitionCell3D
	{
		int32_t X { 0 };
		int32_t Y { 0 };
		int32_t Z { 0 };

		bool operator==(const PartitionCell3D& other) const
		{
			return X == other.X && Y == other.Y && Z == other.Z;
		}
	};
}

namespace std
{
	template <>
	struct hash<World::PartitionCell3D>
	{
		size_t operator()(const World::PartitionCell3D& c) const noexcept
		{
			size_t h = 0x9e3779b9;
			h ^= std::hash<int32_t>()(c.X) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>()(c.Y) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>()(c.Z) + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};
}

namespace World
{
	class WLD_API WorldPartition3D
	{
	public:
		WorldPartition3D(float cellSize = 64.0f, float streamingRadius = 192.0f);
		~WorldPartition3D() = default;

		float GetCellSize() const { return m_CellSize; }
		float GetStreamingRadius() const { return m_StreamingRadius; }

		PartitionCell3D WorldToCell(const glm::dvec3& pos) const;

		// 根据观察者 64 位双精度世界位置计算需要动态装载与卸载的 3D 分区切片
		void UpdateViewer(
			const glm::dvec3& viewerPos,
			std::vector<PartitionCell3D>& outCellsToLoad,
			std::vector<PartitionCell3D>& outCellsToUnload);

		const std::unordered_set<PartitionCell3D>& GetActiveCells() const { return m_ActiveCells; }
		void Clear();

	private:
		float m_CellSize { 64.0f };
		float m_StreamingRadius { 192.0f };
		std::unordered_set<PartitionCell3D> m_ActiveCells;
	};
}
