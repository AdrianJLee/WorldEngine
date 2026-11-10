#pragma once

#include "World/Core/Export.h"
#include <cstdint>
#include <cmath>
#include <glm/glm.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace World::Level
{
	struct ChunkCoord3D
	{
		int32_t x = 0;
		int32_t y = 0;
		int32_t z = 0;

		bool operator==(const ChunkCoord3D& other) const
		{
			return x == other.x && y == other.y && z == other.z;
		}

		uint64_t ToId() const
		{
			uint64_t ux = static_cast<uint64_t>(static_cast<uint32_t>(x));
			uint64_t uy = static_cast<uint64_t>(static_cast<uint32_t>(y));
			uint64_t uz = static_cast<uint64_t>(static_cast<uint32_t>(z));
			return (ux & 0x1FFFFFull) | ((uy & 0x1FFFFFull) << 21) | ((uz & 0x3FFFFFull) << 42);
		}

		static ChunkCoord3D WorldToChunk(const glm::vec3& worldPos, float chunkSize)
		{
			float inv = 1.0f / chunkSize;
			return ChunkCoord3D {
				static_cast<int32_t>(std::floor(worldPos.x * inv)),
				static_cast<int32_t>(std::floor(worldPos.y * inv)),
				static_cast<int32_t>(std::floor(worldPos.z * inv))
			};
		}

		glm::vec3 GetWorldCenter(float chunkSize) const
		{
			return glm::vec3(
				(static_cast<float>(x) + 0.5f) * chunkSize,
				(static_cast<float>(y) + 0.5f) * chunkSize,
				(static_cast<float>(z) + 0.5f) * chunkSize
			);
		}
	};

	struct ChunkCoord3DHash
	{
		std::size_t operator()(const ChunkCoord3D& c) const
		{
			std::size_t h1 = std::hash<int32_t>()(c.x);
			std::size_t h2 = std::hash<int32_t>()(c.y);
			std::size_t h3 = std::hash<int32_t>()(c.z);
			return h1 ^ (h2 << 1) ^ (h3 << 2);
		}
	};

	enum class StreamingLodTier : uint8_t
	{
		Near = 0, // 高精实体与动力学碰撞体
		Mid = 1,  // 静态网格与静态碰撞体
		Far = 2   // 远景 HLOD 与地表高度场
	};

	struct ChunkStreamingEvaluation
	{
		std::vector<ChunkCoord3D> ChunksToLoadNear;
		std::vector<ChunkCoord3D> ChunksToLoadMid;
		std::vector<ChunkCoord3D> ChunksToLoadFar;
		std::vector<ChunkCoord3D> ChunksToUnload;
	};

	class WLD_API SpatialGridManager
	{
	public:
		SpatialGridManager(float chunkSize = 64.0f, float nearRadius = 64.0f, float midRadius = 192.0f, float farRadius = 384.0f);
		~SpatialGridManager();

		void SetChunkSize(float size);
		float GetChunkSize() const { return m_ChunkSize; }

		void SetStreamingRadii(float nearRadius, float midRadius, float farRadius);

		ChunkStreamingEvaluation Evaluate(const glm::vec3& viewerPosition, bool lockZPlane = false);

		bool IsChunkActive(const ChunkCoord3D& coord) const;
		void MarkChunkLoaded(const ChunkCoord3D& coord, StreamingLodTier tier);
		void MarkChunkUnloaded(const ChunkCoord3D& coord);
		size_t ActiveChunkCount() const { return m_ActiveChunks.size(); }

	private:
		float m_ChunkSize = 64.0f;
		float m_NearRadius = 64.0f;
		float m_MidRadius = 192.0f;
		float m_FarRadius = 384.0f;
		std::unordered_map<ChunkCoord3D, StreamingLodTier, ChunkCoord3DHash> m_ActiveChunks;
	};
}
