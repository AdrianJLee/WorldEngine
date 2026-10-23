#include "World/Foliage/Foliage.h"
#include "World/Core/Log.h"
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
		// Play 期间编辑器只读：共享 FoliageData 的写入口统一在这里拦下。
		bool s_FoliageMutationLocked = false;
		bool BlockedByPlayLock(const char* operation)
		{
			if (!s_FoliageMutationLocked)
				return false;
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				WLD_CORE_WARN("[foliage] '{0}' ignored: Play/Simulate is running (editor data is read-only)", operation);
			}
			return true;
		}
	}

	void FoliageData::SetMutationLocked(bool locked) { s_FoliageMutationLocked = locked; }
	bool FoliageData::IsMutationLocked() { return s_FoliageMutationLocked; }
	namespace
	{
		constexpr uint32_t kFoliageMagic = 0x464F4C49; // "FOLI"
		constexpr uint32_t kFoliageVersion = 2;
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
		defaultGrass.Name = "0: 翠绿草丛 (Wild Grass)";
		defaultGrass.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/grass_cluster.wmodel");
		defaultGrass.MaterialAsset.Path = StringPool::Get().InternPath("textures/foliage/grass_tuft.png");
		defaultGrass.Density = 2.0f;
		defaultGrass.ScaleMinMax = glm::vec2(0.8f, 1.2f);
		defaultGrass.RandomYaw = 360.0f;
		defaultGrass.AlignToNormal = true;
		defaultGrass.SlopeLimit = glm::vec2(0.0f, 40.0f);
		defaultGrass.PaintingActive = true;
		defaultGrass.LayerMask = 0x01; // Bit 0: Grass
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

	size_t FoliageData::GetInstanceCountByType(size_t typeIndex) const
	{
		size_t count = 0;
		for (const auto& chunk : m_Chunks)
		{
			for (const auto& inst : chunk.Instances)
			{
				if (static_cast<size_t>(inst.Params.w + 0.5f) == typeIndex)
					++count;
			}
		}
		return count;
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
		const TerrainData* terrain, const glm::mat4& terrainTransform)
	{
		if (BlockedByPlayLock("PaintFoliage"))
			return 0;
		if (typeIndex >= m_Types.size() || radius <= 0.0f)
			return 0;

		const FoliageType& type = m_Types[typeIndex];
		const float area = kPi * radius * radius;
		// 目标区域最大容纳实例数（密度上限，避免按住鼠标无限增加）
		const uint32_t maxAllowed = std::max(1u, static_cast<uint32_t>(area * density * 0.15f));

		const float r2 = radius * radius;
		uint32_t currentCount = 0;
		for (const auto& chunk : m_Chunks)
		{
			if (center.x + radius < chunk.BoundsMin.x || center.x - radius > chunk.BoundsMax.x ||
				center.z + radius < chunk.BoundsMin.z || center.z - radius > chunk.BoundsMax.z)
				continue;

			for (const auto& inst : chunk.Instances)
			{
				if (static_cast<int32_t>(inst.Params.w) == static_cast<int32_t>(typeIndex))
				{
					const glm::vec3 pos(inst.Transform[3]);
					const float dx = pos.x - center.x;
					const float dz = pos.z - center.z;
					if (dx * dx + dz * dz <= r2)
						++currentCount;
				}
			}
		}

		if (currentCount >= maxAllowed)
			return 0;

		const uint32_t budget = maxAllowed - currentCount;
		const uint32_t candidateCount = std::min(budget, std::max(1u, static_cast<uint32_t>(maxAllowed * 0.35f)));

		// 泊松防重叠最小间距（基于密度自适应），彻底杜绝同点重叠导致的 Z-fighting 闪烁
		const float minDist = std::max(0.6f, 1.6f / std::sqrt(std::max(0.1f, density)));
		const float minDist2 = minDist * minDist;

		static std::mt19937 rng(1337);
		std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

		uint32_t spawned = 0;
		const glm::mat4 invTerrain = glm::inverse(terrainTransform);

		for (uint32_t i = 0; i < candidateCount; ++i)
		{
			const float r = radius * std::sqrt(dist01(rng));
			const float theta = dist01(rng) * 2.0f * kPi;
			float worldX = center.x + r * std::cos(theta);
			float worldZ = center.z + r * std::sin(theta);
			float worldY = center.y;
			glm::vec3 worldNormal(0.0f, 1.0f, 0.0f);

			if (terrain)
			{
				const glm::vec3 localPos = glm::vec3(invTerrain * glm::vec4(worldX, 0.0f, worldZ, 1.0f));
				if (localPos.x < 0.0f || localPos.x > terrain->GetWorldWidth() ||
					localPos.z < 0.0f || localPos.z > terrain->GetWorldHeight())
					continue;

				const uint32_t gx = std::clamp(static_cast<uint32_t>(localPos.x / terrain->GetCellSize()), 0u, terrain->GetWidth() - 1);
				const uint32_t gz = std::clamp(static_cast<uint32_t>(localPos.z / terrain->GetCellSize()), 0u, terrain->GetHeight() - 1);
				if (terrain->IsHole(gx, gz))
					continue;

				// 地貌材质层 LayerMask 过滤 (Bit 0=Layer0/Grass, Bit 1=Layer1/Dirt, Bit 2=Layer2/Rock, Bit 3=Layer3/Sand)
				if (type.LayerMask != 0xFFFFFFFF)
				{
					const glm::vec4 weights = terrain->GetSplatWeights(gx, gz);
					float allowedWeightSum = 0.0f;
					if (type.LayerMask & (1u << 0)) allowedWeightSum += weights.x;
					if (type.LayerMask & (1u << 1)) allowedWeightSum += weights.y;
					if (type.LayerMask & (1u << 2)) allowedWeightSum += weights.z;
					if (type.LayerMask & (1u << 3)) allowedWeightSum += weights.w;

					if (allowedWeightSum < 0.25f)
						continue;
				}

				const float localHeight = terrain->SampleHeight(localPos.x, localPos.z);
				const glm::vec3 localNormal = terrain->SampleNormal(localPos.x, localPos.z);

				// 坡度限制过滤
				const float slopeDeg = std::acos(std::clamp(localNormal.y, 0.0f, 1.0f)) * (180.0f / kPi);
				if (slopeDeg < type.SlopeLimit.x || slopeDeg > type.SlopeLimit.y)
					continue;

				const glm::vec4 sampledWorld = terrainTransform * glm::vec4(localPos.x, localHeight, localPos.z, 1.0f);
				worldX = sampledWorld.x;
				worldY = sampledWorld.y;
				worldZ = sampledWorld.z;
				worldNormal = glm::normalize(glm::mat3(terrainTransform) * localNormal);

				// 高度限制过滤 (世界高度)
				if (worldY < type.HeightLimit.x || worldY > type.HeightLimit.y)
					continue;
			}

			// 随机缩放与偏航角
			const float scale = type.ScaleMinMax.x + (type.ScaleMinMax.y - type.ScaleMinMax.x) * dist01(rng);
			const float yaw = dist01(rng) * type.RandomYaw;
			const float pitch = (dist01(rng) * 2.0f - 1.0f) * type.PitchVariation;

			glm::mat4 xform = AlignOrientation(type.AlignToNormal ? worldNormal : glm::vec3(0, 1, 0), yaw, pitch, scale);
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

			// 检查邻近实例距离，避免同位置扎堆重叠
			bool tooClose = false;
			for (const auto& existing : chunk.Instances)
			{
				const glm::vec3 ep(existing.Transform[3]);
				const float edx = ep.x - worldX;
				const float edz = ep.z - worldZ;
				if (edx * edx + edz * edz < minDist2)
				{
					tooClose = true;
					break;
				}
			}
			if (tooClose)
				continue;

			chunk.Instances.push_back(inst);
			chunk.DirtyBuffer = true;
			++spawned;
		}

		return spawned;
	}

	uint32_t FoliageData::PaintFoliageMulti(const glm::vec3& center, float radius, float brushDensity,
		const std::vector<size_t>& activeTypes,
		const TerrainData* terrain, const glm::mat4& terrainTransform)
	{
		if (BlockedByPlayLock("PaintFoliageMulti"))
			return 0;
		if (activeTypes.empty() || radius <= 0.0f || brushDensity <= 0.0f)
			return 0;

		std::vector<size_t> validActive;
		validActive.reserve(activeTypes.size());
		for (size_t idx : activeTypes)
		{
			if (idx < m_Types.size() && m_Types[idx].PaintingActive)
			{
				validActive.push_back(idx);
			}
		}

		if (validActive.empty())
			return 0;

		if (validActive.size() == 1)
			return PaintFoliage(center, radius, brushDensity, validActive[0], terrain, terrainTransform);

		float totalDensityWeight = 0.0f;
		for (size_t idx : validActive)
		{
			totalDensityWeight += std::max(0.01f, m_Types[idx].Density);
		}
		if (totalDensityWeight <= 0.0f)
			totalDensityWeight = static_cast<float>(validActive.size());

		uint32_t totalSpawned = 0;
		for (size_t idx : validActive)
		{
			const float weight = std::max(0.01f, m_Types[idx].Density) / totalDensityWeight;
			const float effectiveDensity = brushDensity * weight;
			totalSpawned += PaintFoliage(center, radius, effectiveDensity, idx, terrain, terrainTransform);
		}

		return totalSpawned;
	}

	uint32_t FoliageData::EraseFoliage(const glm::vec3& center, float radius, int32_t typeIndex)
	{
		if (BlockedByPlayLock("EraseFoliage"))
			return 0;
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

	void FoliageData::RealignToTerrain(const TerrainData* terrain, const glm::mat4& terrainTransform)
	{
		if (BlockedByPlayLock("RealignToTerrain"))
			return;
		if (!terrain)
			return;

		const glm::mat4 invTerrain = glm::inverse(terrainTransform);
		const glm::mat3 normMat = glm::mat3(terrainTransform);

		for (auto& chunk : m_Chunks)
		{
			bool chunkModified = false;
			for (auto& inst : chunk.Instances)
			{
				const size_t typeIdx = static_cast<size_t>(inst.Params.w + 0.5f);
				if (typeIdx >= m_Types.size())
					continue;

				const FoliageType& type = m_Types[typeIdx];
				const glm::vec3 worldPos(inst.Transform[3]);
				const glm::vec3 localPos = glm::vec3(invTerrain * glm::vec4(worldPos.x, 0.0f, worldPos.z, 1.0f));

				if (localPos.x < 0.0f || localPos.x > terrain->GetWorldWidth() ||
					localPos.z < 0.0f || localPos.z > terrain->GetWorldHeight())
					continue;

				const float localHeight = terrain->SampleHeight(localPos.x, localPos.z);
				const glm::vec3 localNormal = terrain->SampleNormal(localPos.x, localPos.z);

				const glm::vec4 sampledWorld = terrainTransform * glm::vec4(localPos.x, localHeight, localPos.z, 1.0f);
				const glm::vec3 worldNormal = glm::normalize(normMat * localNormal);

				// 更新世界坐标
				inst.Transform[3] = glm::vec4(sampledWorld.x, sampledWorld.y, sampledWorld.z, 1.0f);

				// 更新对齐法线旋转
				const glm::vec3 targetUp = type.AlignToNormal ? worldNormal : glm::vec3(0.0f, 1.0f, 0.0f);
				const glm::vec3 currentUp = glm::normalize(glm::vec3(inst.Transform[1]));
				const float dotUp = glm::clamp(glm::dot(currentUp, targetUp), -1.0f, 1.0f);

				if (dotUp < 0.9999f && dotUp > -0.9999f)
				{
					const glm::vec3 axis = glm::normalize(glm::cross(currentUp, targetUp));
					const float angle = std::acos(dotUp);
					const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), angle, axis);
					const glm::mat3 newLinear = glm::mat3(rot) * glm::mat3(inst.Transform);
					inst.Transform[0] = glm::vec4(newLinear[0], 0.0f);
					inst.Transform[1] = glm::vec4(newLinear[1], 0.0f);
					inst.Transform[2] = glm::vec4(newLinear[2], 0.0f);
				}
				else if (dotUp <= -0.9999f)
				{
					glm::vec3 ortho = std::abs(currentUp.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
					const glm::vec3 axis = glm::normalize(glm::cross(currentUp, ortho));
					const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), kPi, axis);
					const glm::mat3 newLinear = glm::mat3(rot) * glm::mat3(inst.Transform);
					inst.Transform[0] = glm::vec4(newLinear[0], 0.0f);
					inst.Transform[1] = glm::vec4(newLinear[1], 0.0f);
					inst.Transform[2] = glm::vec4(newLinear[2], 0.0f);
				}

				chunkModified = true;
			}

			if (chunkModified)
			{
				chunk.DirtyBuffer = true;
			}
		}
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
		if (BlockedByPlayLock("Clear"))
			return;
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

			const uint8_t paintingActive = type.PaintingActive ? 1 : 0;
			stream.write(reinterpret_cast<const char*>(&paintingActive), sizeof(paintingActive));
			stream.write(reinterpret_cast<const char*>(&type.LayerMask), sizeof(type.LayerMask));
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
		if (magic != kFoliageMagic || version == 0 || version > kFoliageVersion)
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

			if (!type.MeshAsset.Path.IsValid() || StringPool::Get().PathOf(type.MeshAsset.Path).empty())
			{
				type.MeshAsset.Path = StringPool::Get().InternPath(i == 2 ? "models/foliage/bush_cluster.wmodel" : (i == 3 ? "models/foliage/scatter_pebble.wmodel" : "models/foliage/grass_cluster.wmodel"));
			}
			if (!type.MaterialAsset.Path.IsValid() || StringPool::Get().PathOf(type.MaterialAsset.Path).empty())
			{
				type.MaterialAsset.Path = StringPool::Get().InternPath(i == 1 ? "textures/foliage/wild_flower.png" : (i == 3 ? "textures/terrain/rock_albedo.png" : "textures/foliage/grass_tuft.png"));
			}

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

			if (version >= 2)
			{
				uint8_t paintingActive = 1;
				stream.read(reinterpret_cast<char*>(&paintingActive), sizeof(paintingActive));
				type.PaintingActive = (paintingActive != 0);
				stream.read(reinterpret_cast<char*>(&type.LayerMask), sizeof(type.LayerMask));
			}
			else
			{
				type.PaintingActive = true;
				type.LayerMask = 0xFFFFFFFF;
			}
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

