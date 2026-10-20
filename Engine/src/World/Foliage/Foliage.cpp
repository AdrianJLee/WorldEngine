#include "World/Foliage/Foliage.h"
#include "World/Core/StringPool.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>

namespace World
{
	namespace
	{
		constexpr uint32_t kFoliageMagic = 0x464F4C49; // "FOLI"
		constexpr uint32_t kFoliageVersion = 1;
		constexpr float kPi = 3.14159265358979323846f;

		glm::mat4 AlignOrientation(const glm::vec3& normal, float yawDeg, float pitchDeg, float scale)
		{
			glm::mat4 model = glm::mat4(1.0f);
			glm::vec3 up = glm::normalize(normal);
			if (std::abs(up.y) < 0.999f)
			{
				glm::vec3 defaultUp(0.0f, 1.0f, 0.0f);
				glm::vec3 axis = glm::cross(defaultUp, up);
				float angle = std::acos(std::clamp(glm::dot(defaultUp, up), -1.0f, 1.0f));
				model = glm::rotate(model, angle, glm::normalize(axis));
			}
			model = glm::rotate(model, glm::radians(yawDeg), up);
			if (std::abs(pitchDeg) > 0.001f)
			{
				glm::vec3 right = glm::cross(up, glm::vec3(0, 0, 1));
				if (glm::length(right) > 0.001f)
					model = glm::rotate(model, glm::radians(pitchDeg), glm::normalize(right));
			}
			model = glm::scale(model, glm::vec3(scale));
			return model;
		}
	}

	FoliageData::FoliageData()
	{
		FoliageType defaultGrass;
		defaultGrass.Name = "Default Grass";
		defaultGrass.Density = 2.0f;
		defaultGrass.ScaleMinMax = glm::vec2(0.8f, 1.2f);
		defaultGrass.RandomYaw = 360.0f;
		defaultGrass.AlignToNormal = true;
		defaultGrass.SlopeLimit = glm::vec2(0.0f, 40.0f);
		m_Types.push_back(defaultGrass);
	}

	size_t FoliageData::AddType(const FoliageType& type)
	{
		m_Types.push_back(type);
		return m_Types.size() - 1;
	}

	void FoliageData::RemoveType(size_t index)
	{
		if (index < m_Types.size())
		{
			m_Types.erase(m_Types.begin() + index);
			// 清除该类型的实例
			for (auto& chunk : m_Chunks)
			{
				chunk.Instances.erase(
					std::remove_if(chunk.Instances.begin(), chunk.Instances.end(),
						[index](const FoliageInstanceData& inst) {
							return static_cast<size_t>(inst.Params.w) == index;
						}),
					chunk.Instances.end());
				chunk.DirtyBuffer = true;
			}
		}
	}

	size_t FoliageData::GetTotalInstanceCount() const
	{
		size_t total = 0;
		for (const auto& chunk : m_Chunks)
			total += chunk.Instances.size();
		return total;
	}

	FoliageChunk& FoliageData::GetOrCreateChunk(int32_t cx, int32_t cz)
	{
		for (auto& chunk : m_Chunks)
		{
			if (chunk.ChunkX == cx && chunk.ChunkZ == cz)
				return chunk;
		}

		FoliageChunk newChunk;
		newChunk.ChunkX = cx;
		newChunk.ChunkZ = cz;
		newChunk.BoundsMin = glm::vec3(static_cast<float>(cx) * m_ChunkSize, -1000.0f, static_cast<float>(cz) * m_ChunkSize);
		newChunk.BoundsMax = glm::vec3(static_cast<float>(cx + 1) * m_ChunkSize, 1000.0f, static_cast<float>(cz + 1) * m_ChunkSize);
		newChunk.DirtyBuffer = true;
		m_Chunks.push_back(std::move(newChunk));
		return m_Chunks.back();
	}

	uint32_t FoliageData::PaintFoliage(const glm::vec3& center, float radius, float density, size_t typeIndex,
		const TerrainData* terrain)
	{
		if (typeIndex >= m_Types.size() || radius <= 0.0f)
			return 0;

		const FoliageType& type = m_Types[typeIndex];
		const float area = kPi * radius * radius;
		const uint32_t candidateCount = std::max(1u, static_cast<uint32_t>(area * density * 0.15f));

		static std::mt19937 rng(1337);
		std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

		uint32_t spawned = 0;

		for (uint32_t i = 0; i < candidateCount; ++i)
		{
			const float r = radius * std::sqrt(dist01(rng));
			const float theta = dist01(rng) * 2.0f * kPi;
			const float worldX = center.x + r * std::cos(theta);
			const float worldZ = center.z + r * std::sin(theta);
			float worldY = center.y;
			glm::vec3 normal(0.0f, 1.0f, 0.0f);

			if (terrain)
			{
				if (worldX < 0.0f || worldX > terrain->GetWorldWidth() ||
					worldZ < 0.0f || worldZ > terrain->GetWorldHeight())
					continue;

				const uint32_t gx = std::clamp(static_cast<uint32_t>(worldX / terrain->GetCellSize()), 0u, terrain->GetWidth() - 1);
				const uint32_t gz = std::clamp(static_cast<uint32_t>(worldZ / terrain->GetCellSize()), 0u, terrain->GetHeight() - 1);
				if (terrain->IsHole(gx, gz))
					continue;

				worldY = terrain->SampleHeight(worldX, worldZ);
				normal = terrain->SampleNormal(worldX, worldZ);

				// 坡度限制过滤
				const float slopeDeg = std::acos(std::clamp(normal.y, 0.0f, 1.0f)) * (180.0f / kPi);
				if (slopeDeg < type.SlopeLimit.x || slopeDeg > type.SlopeLimit.y)
					continue;

				// 高度限制过滤
				if (worldY < type.HeightLimit.x || worldY > type.HeightLimit.y)
					continue;
			}

			// 随机缩放与偏航角
			const float scale = type.ScaleMinMax.x + (type.ScaleMinMax.y - type.ScaleMinMax.x) * dist01(rng);
			const float yaw = dist01(rng) * type.RandomYaw;
			const float pitch = (dist01(rng) * 2.0f - 1.0f) * type.PitchVariation;

			glm::mat4 xform = AlignOrientation(type.AlignToNormal ? normal : glm::vec3(0, 1, 0), yaw, pitch, scale);
			xform[3] = glm::vec4(worldX, worldY, worldZ, 1.0f);

			FoliageInstanceData inst;
			inst.Transform = xform;
			inst.Params = glm::vec4(
				dist01(rng) * 2.0f * kPi,     // windPhase
				dist01(rng),                  // ditherOffset
				scale,                        // scale
				static_cast<float>(typeIndex) // typeIndex
			);

			const int32_t cx = static_cast<int32_t>(std::floor(worldX / m_ChunkSize));
			const int32_t cz = static_cast<int32_t>(std::floor(worldZ / m_ChunkSize));
			FoliageChunk& chunk = GetOrCreateChunk(cx, cz);
			chunk.Instances.push_back(inst);
			chunk.DirtyBuffer = true;
			++spawned;
		}

		return spawned;
	}

	uint32_t FoliageData::EraseFoliage(const glm::vec3& center, float radius, int32_t typeIndex)
	{
		if (radius <= 0.0f)
			return 0;

		const float r2 = radius * radius;
		uint32_t erased = 0;

		for (auto& chunk : m_Chunks)
		{
			const size_t before = chunk.Instances.size();
			chunk.Instances.erase(
				std::remove_if(chunk.Instances.begin(), chunk.Instances.end(),
					[&](const FoliageInstanceData& inst) {
						if (typeIndex >= 0 && static_cast<int32_t>(inst.Params.w) != typeIndex)
							return false;

						const glm::vec3 pos(inst.Transform[3]);
						const float dx = pos.x - center.x;
						const float dz = pos.z - center.z;
						return (dx * dx + dz * dz) <= r2;
					}),
				chunk.Instances.end());

			if (chunk.Instances.size() != before)
			{
				erased += static_cast<uint32_t>(before - chunk.Instances.size());
				chunk.DirtyBuffer = true;
			}
		}

		return erased;
	}

	void FoliageData::CollectVisibleInstances(const glm::vec3& cameraPos, const glm::mat4&,
		float maxDistance, float,
		std::vector<FoliageInstanceData>& outInstances) const
	{
		const float maxDist2 = maxDistance * maxDistance;

		for (const auto& chunk : m_Chunks)
		{
			// 粗粒度 Chunk 距离过滤
			const glm::vec3 chunkCenter = (chunk.BoundsMin + chunk.BoundsMax) * 0.5f;
			const glm::vec2 dCenter = glm::vec2(chunkCenter.x, chunkCenter.z) - glm::vec2(cameraPos.x, cameraPos.z);
			const float distCenter2 = glm::dot(dCenter, dCenter);
			if (distCenter2 > (maxDistance + m_ChunkSize) * (maxDistance + m_ChunkSize))
				continue;

			for (const auto& inst : chunk.Instances)
			{
				const glm::vec3 pos(inst.Transform[3]);
				const glm::vec2 dPos = glm::vec2(pos.x, pos.z) - glm::vec2(cameraPos.x, cameraPos.z);
				const float d2 = glm::dot(dPos, dPos);
				if (d2 <= maxDist2)
				{
					outInstances.push_back(inst);
				}
			}
		}
	}

	void FoliageData::Clear()
	{
		m_Chunks.clear();
	}

	bool FoliageData::SaveToFile(const std::string& path) const
	{
		std::ofstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return false;

		stream.write(reinterpret_cast<const char*>(&kFoliageMagic), sizeof(kFoliageMagic));
		stream.write(reinterpret_cast<const char*>(&kFoliageVersion), sizeof(kFoliageVersion));

		const uint32_t typeCount = static_cast<uint32_t>(m_Types.size());
		stream.write(reinterpret_cast<const char*>(&typeCount), sizeof(typeCount));

		for (const auto& type : m_Types)
		{
			const uint32_t nameLen = static_cast<uint32_t>(type.Name.size());
			stream.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
			if (nameLen > 0)
				stream.write(type.Name.data(), nameLen);

			const std::string meshPath = std::string(StringPool::Get().PathOf(type.MeshAsset.Path));
			const uint32_t meshLen = static_cast<uint32_t>(meshPath.size());
			stream.write(reinterpret_cast<const char*>(&meshLen), sizeof(meshLen));
			if (meshLen > 0)
				stream.write(meshPath.data(), meshLen);
			const uint64_t meshId = type.MeshAsset.Identity.Value;
			stream.write(reinterpret_cast<const char*>(&meshId), sizeof(meshId));

			const std::string matPath = std::string(StringPool::Get().PathOf(type.MaterialAsset.Path));
			const uint32_t matLen = static_cast<uint32_t>(matPath.size());
			stream.write(reinterpret_cast<const char*>(&matLen), sizeof(matLen));
			if (matLen > 0)
				stream.write(matPath.data(), matLen);
			const uint64_t matId = type.MaterialAsset.Identity.Value;
			stream.write(reinterpret_cast<const char*>(&matId), sizeof(matId));

			stream.write(reinterpret_cast<const char*>(&type.Density), sizeof(type.Density));
			stream.write(reinterpret_cast<const char*>(&type.ScaleMinMax), sizeof(type.ScaleMinMax));
			stream.write(reinterpret_cast<const char*>(&type.RandomYaw), sizeof(type.RandomYaw));
			stream.write(reinterpret_cast<const char*>(&type.PitchVariation), sizeof(type.PitchVariation));
			stream.write(reinterpret_cast<const char*>(&type.SlopeLimit), sizeof(type.SlopeLimit));
			stream.write(reinterpret_cast<const char*>(&type.HeightLimit), sizeof(type.HeightLimit));
			stream.write(reinterpret_cast<const char*>(&type.CullDistance), sizeof(type.CullDistance));
			stream.write(reinterpret_cast<const char*>(&type.WindMultiplier), sizeof(type.WindMultiplier));
			const uint8_t alignNorm = type.AlignToNormal ? 1 : 0;
			stream.write(reinterpret_cast<const char*>(&alignNorm), sizeof(alignNorm));
		}

		const uint32_t chunkCount = static_cast<uint32_t>(m_Chunks.size());
		stream.write(reinterpret_cast<const char*>(&chunkCount), sizeof(chunkCount));

		for (const auto& chunk : m_Chunks)
		{
			stream.write(reinterpret_cast<const char*>(&chunk.ChunkX), sizeof(chunk.ChunkX));
			stream.write(reinterpret_cast<const char*>(&chunk.ChunkZ), sizeof(chunk.ChunkZ));
			stream.write(reinterpret_cast<const char*>(&chunk.BoundsMin), sizeof(chunk.BoundsMin));
			stream.write(reinterpret_cast<const char*>(&chunk.BoundsMax), sizeof(chunk.BoundsMax));

			const uint32_t instCount = static_cast<uint32_t>(chunk.Instances.size());
			stream.write(reinterpret_cast<const char*>(&instCount), sizeof(instCount));
			if (instCount > 0)
			{
				stream.write(reinterpret_cast<const char*>(chunk.Instances.data()),
					instCount * sizeof(FoliageInstanceData));
			}
		}

		return true;
	}

	bool FoliageData::LoadFromFile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return false;

		uint32_t magic = 0;
		uint32_t version = 0;
		stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
		stream.read(reinterpret_cast<char*>(&version), sizeof(version));
		if (magic != kFoliageMagic || version != kFoliageVersion)
			return false;

		uint32_t typeCount = 0;
		stream.read(reinterpret_cast<char*>(&typeCount), sizeof(typeCount));
		m_Types.clear();
		m_Types.resize(typeCount);

		for (uint32_t i = 0; i < typeCount; ++i)
		{
			FoliageType& type = m_Types[i];
			uint32_t nameLen = 0;
			stream.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
			if (nameLen > 0)
			{
				type.Name.resize(nameLen);
				stream.read(type.Name.data(), nameLen);
			}

			uint32_t meshLen = 0;
			stream.read(reinterpret_cast<char*>(&meshLen), sizeof(meshLen));
			std::string meshPath;
			if (meshLen > 0)
			{
				meshPath.resize(meshLen);
				stream.read(meshPath.data(), meshLen);
			}
			uint64_t meshId = 0;
			stream.read(reinterpret_cast<char*>(&meshId), sizeof(meshId));
			type.MeshAsset.Path = meshPath.empty() ? PathId() : StringPool::Get().InternPath(meshPath);
			type.MeshAsset.Identity = AssetId { meshId };

			uint32_t matLen = 0;
			stream.read(reinterpret_cast<char*>(&matLen), sizeof(matLen));
			std::string matPath;
			if (matLen > 0)
			{
				matPath.resize(matLen);
				stream.read(matPath.data(), matLen);
			}
			uint64_t matId = 0;
			stream.read(reinterpret_cast<char*>(&matId), sizeof(matId));
			type.MaterialAsset.Path = matPath.empty() ? PathId() : StringPool::Get().InternPath(matPath);
			type.MaterialAsset.Identity = AssetId { matId };

			stream.read(reinterpret_cast<char*>(&type.Density), sizeof(type.Density));
			stream.read(reinterpret_cast<char*>(&type.ScaleMinMax), sizeof(type.ScaleMinMax));
			stream.read(reinterpret_cast<char*>(&type.RandomYaw), sizeof(type.RandomYaw));
			stream.read(reinterpret_cast<char*>(&type.PitchVariation), sizeof(type.PitchVariation));
			stream.read(reinterpret_cast<char*>(&type.SlopeLimit), sizeof(type.SlopeLimit));
			stream.read(reinterpret_cast<char*>(&type.HeightLimit), sizeof(type.HeightLimit));
			stream.read(reinterpret_cast<char*>(&type.CullDistance), sizeof(type.CullDistance));
			stream.read(reinterpret_cast<char*>(&type.WindMultiplier), sizeof(type.WindMultiplier));
			uint8_t alignNorm = 0;
			stream.read(reinterpret_cast<char*>(&alignNorm), sizeof(alignNorm));
			type.AlignToNormal = (alignNorm != 0);
		}

		uint32_t chunkCount = 0;
		stream.read(reinterpret_cast<char*>(&chunkCount), sizeof(chunkCount));
		m_Chunks.clear();
		m_Chunks.resize(chunkCount);

		for (uint32_t i = 0; i < chunkCount; ++i)
		{
			FoliageChunk& chunk = m_Chunks[i];
			stream.read(reinterpret_cast<char*>(&chunk.ChunkX), sizeof(chunk.ChunkX));
			stream.read(reinterpret_cast<char*>(&chunk.ChunkZ), sizeof(chunk.ChunkZ));
			stream.read(reinterpret_cast<char*>(&chunk.BoundsMin), sizeof(chunk.BoundsMin));
			stream.read(reinterpret_cast<char*>(&chunk.BoundsMax), sizeof(chunk.BoundsMax));

			uint32_t instCount = 0;
			stream.read(reinterpret_cast<char*>(&instCount), sizeof(instCount));
			chunk.Instances.resize(instCount);
			if (instCount > 0)
			{
				stream.read(reinterpret_cast<char*>(chunk.Instances.data()),
					instCount * sizeof(FoliageInstanceData));
			}
			chunk.DirtyBuffer = true;
		}

		return true;
	}
}

