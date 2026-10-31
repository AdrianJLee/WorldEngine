#include "wldpch.h"
#include "World/Environment/PCGBiome.h"
#include "World/Foliage/Foliage.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <glm/gtc/matrix_transform.hpp>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
	}

	BiomeType PCGBiomeGenerator::ClassifyBiome(float elevation, float slopeDeg, float moisture)
	{
		if (elevation > 150.0f)
			return BiomeType::SnowPeak;

		if (slopeDeg > 40.0f)
			return BiomeType::RockyMountain;

		if (moisture < 0.25f)
			return BiomeType::Desert;

		if (moisture >= 0.5f && elevation < 100.0f)
			return BiomeType::Forest;

		return BiomeType::Meadow;
	}

	void PCGBiomeGenerator::GenerateBiomePoints(
		const TerrainData& terrain,
		const BiomePCGComponent& comp,
		std::vector<PCGSpawnPoint>& outPoints,
		const glm::mat4& terrainWorld)
	{
		outPoints.clear();
		const float w = terrain.GetWorldWidth();
		const float h = terrain.GetWorldHeight();

		// 自然步长 (2.5m 形成细腻繁茂覆盖，杜绝 8m 巨大方格阵)
		const float spacing = 2.5f;
		const uint32_t stepsX = static_cast<uint32_t>(w / spacing);
		const uint32_t stepsZ = static_cast<uint32_t>(h / spacing);

		for (uint32_t z = 0; z < stepsZ; ++z)
		{
			for (uint32_t x = 0; x < stepsX; ++x)
			{
				// 伪随机空间抖动 (Jittered Poisson-like Sampling)，彻底打破机械棋盘格
				float hash1 = std::sin(static_cast<float>(x) * 12.9898f + static_cast<float>(z) * 78.233f + static_cast<float>(comp.Seed)) * 43758.5453f;
				hash1 = hash1 - std::floor(hash1);
				float hash2 = std::sin(static_cast<float>(x) * 39.346f + static_cast<float>(z) * 11.135f + static_cast<float>(comp.Seed)) * 43758.5453f;
				hash2 = hash2 - std::floor(hash2);

				const float jitterX = (hash1 - 0.5f) * spacing * 0.85f;
				const float jitterZ = (hash2 - 0.5f) * spacing * 0.85f;

				const float lx = std::clamp(static_cast<float>(x) * spacing + jitterX, 0.0f, w);
				const float lz = std::clamp(static_cast<float>(z) * spacing + jitterZ, 0.0f, h);

				const float elev = terrain.SampleHeight(lx, lz);
				if (elev < comp.MinElevation || elev > comp.MaxElevation)
					continue;

				const glm::vec3 localNormal = terrain.SampleNormal(lx, lz);
				const float slopeDeg = std::acos(std::clamp(localNormal.y, 0.0f, 1.0f)) * (180.0f / kPi);
				if (slopeDeg > comp.MaxSlope)
					continue;

				// 多八度分形湿度计算 (Fractal Moisture Noise)
				const float n1 = std::sin(lx * 0.035f + static_cast<float>(comp.Seed)) * std::cos(lz * 0.035f);
				const float n2 = std::sin(lx * 0.085f + static_cast<float>(comp.Seed * 3)) * std::cos(lz * 0.085f) * 0.4f;
				const float moisture = std::clamp(0.5f + 0.5f * (n1 + n2) / 1.4f, 0.0f, 1.0f);
				if (moisture < comp.MoistureThreshold)
					continue;

				PCGSpawnPoint pt;
				pt.Position = glm::vec3(terrainWorld * glm::vec4(lx, elev, lz, 1.0f));
				pt.Normal = glm::normalize(glm::mat3(terrainWorld) * localNormal);
				pt.Biome = ClassifyBiome(elev, slopeDeg, moisture);
				pt.Weight = moisture;

				outPoints.push_back(pt);
			}
		}
	}

	void PCGBiomeGenerator::DeformTerrainAlongSpline(
		TerrainData& terrain,
		const Spline3D& spline,
		float roadWidth,
		float cutDepth,
		float slopeTransition,
		const glm::mat4& terrainWorld)
	{
		const float totalLen = spline.GetTotalLength();
		if (totalLen <= 1.0f || roadWidth <= 0.5f)
			return;

		const float step = roadWidth * 0.35f;
		const uint32_t numSamples = static_cast<uint32_t>(totalLen / step);
		const float totalInfluenceRadius = roadWidth * 0.5f + slopeTransition;

		glm::mat4 invTerrain = glm::inverse(terrainWorld);

		for (uint32_t i = 0; i <= numSamples; ++i)
		{
			const float d = std::min(static_cast<float>(i) * step, totalLen);
			glm::vec3 splinePos, tangent;
			spline.SampleAtDistance(d, splinePos, tangent);

			glm::vec3 localPos = glm::vec3(invTerrain * glm::vec4(splinePos, 1.0f));
			const float targetY = localPos.y - cutDepth;
			terrain.SculptFlatten(localPos.x, localPos.z, totalInfluenceRadius, targetY, 0.9f, TerrainBrushFalloff::Smooth);
		}
	}

	uint32_t PCGBiomeGenerator::PopulateFoliageFromBiomePoints(
		const std::vector<PCGSpawnPoint>& points,
		FoliageData& foliageData,
		float scale)
	{
		if (points.empty())
			return 0;

		if (foliageData.GetTypeCount() == 0)
		{
			FoliageType grass;
			grass.Name = "0: 翠绿草丛 (Wild Grass)";
			grass.MeshAsset = AssetRef::FromPath("models/foliage/grass_cluster.wmodel");
			grass.MaterialAsset = AssetRef::FromPath("textures/foliage/grass_tuft.png");
			grass.Density = 1.0f;
			foliageData.AddType(grass);

			FoliageType flower;
			flower.Name = "1: 雏菊野花 (Wild Flowers)";
			flower.MeshAsset = AssetRef::FromPath("models/foliage/grass_cluster.wmodel");
			flower.MaterialAsset = AssetRef::FromPath("textures/foliage/wild_flower.png");
			flower.Density = 0.8f;
			foliageData.AddType(flower);

			FoliageType shrub;
			shrub.Name = "2: 矮生灌木 (Forest Shrub)";
			shrub.MeshAsset = AssetRef::FromPath("models/foliage/bush_cluster.wmodel");
			shrub.MaterialAsset = AssetRef::FromPath("textures/foliage/bush_leaves.png");
			shrub.Density = 0.5f;
			foliageData.AddType(shrub);

			FoliageType rock;
			rock.Name = "3: 地表碎石 (Scatter Rocks)";
			rock.MeshAsset = AssetRef::FromPath("models/foliage/scatter_pebble.wmodel");
			rock.MaterialAsset = AssetRef::FromPath("textures/terrain/rock_albedo.png");
			rock.Density = 0.6f;
			foliageData.AddType(rock);
		}

		const size_t typeCount = foliageData.GetTypeCount();
		if (typeCount == 0)
			return 0;

		uint32_t spawnedCount = 0;
		std::mt19937 rng(1337);
		std::uniform_real_distribution<float> angleDist(0.0f, 6.2831853f);
		std::uniform_real_distribution<float> scaleDist(0.85f, 1.25f);
		std::uniform_real_distribution<float> rollDist(0.0f, 1.0f);

		for (const auto& pt : points)
		{
			size_t chosenType = 0;
			float r = rollDist(rng);
			switch (pt.Biome)
			{
			case BiomeType::Forest:
				chosenType = (r < 0.65f) ? 0 : 2; // 65% 草丛, 35% 灌木
				break;
			case BiomeType::Meadow:
				chosenType = (r < 0.55f) ? 0 : 1; // 55% 草丛, 45% 野花
				break;
			case BiomeType::RockyMountain:
				chosenType = (r < 0.70f) ? 3 : 2; // 70% 碎石, 30% 灌木
				break;
			case BiomeType::Desert:
				chosenType = (r < 0.80f) ? 3 : 2; // 80% 碎石, 20% 灌木
				break;
			case BiomeType::SnowPeak:
				chosenType = 3; // 碎石
				break;
			default:
				chosenType = 0;
				break;
			}

			if (chosenType >= typeCount)
				chosenType = 0;

			const float s = scale * scaleDist(rng);
			const float yaw = angleDist(rng);

			// 向光性：植物主干主要垂直向上生长，仅带 30% 坡度法线倾角，杜绝悬崖侧壁横向生硬穿刺
			glm::vec3 plantNormal = glm::normalize(glm::mix(glm::vec3(0.0f, 1.0f, 0.0f), pt.Normal, 0.30f));
			glm::mat4 model(1.0f);
			model = glm::translate(model, pt.Position);
			model = glm::rotate(model, yaw, plantNormal);
			glm::vec3 up(0.0f, 1.0f, 0.0f);
			if (glm::length(plantNormal - up) > 1e-4f && glm::length(plantNormal + up) > 1e-4f)
			{
				glm::vec3 axis = glm::normalize(glm::cross(up, plantNormal));
				float angle = std::acos(std::clamp(glm::dot(up, plantNormal), -1.0f, 1.0f));
				model = glm::rotate(model, angle, axis);
			}
			model = glm::scale(model, glm::vec3(s));

			FoliageInstanceData inst;
			inst.Transform = model;
			inst.Params = glm::vec4(angleDist(rng), rollDist(rng), s, static_cast<float>(chosenType));

			const float chunkSize = 32.0f;
			int32_t cx = static_cast<int32_t>(std::floor(pt.Position.x / chunkSize));
			int32_t cz = static_cast<int32_t>(std::floor(pt.Position.z / chunkSize));

			bool chunkFound = false;
			for (auto& chunk : foliageData.GetChunksMut())
			{
				if (chunk.ChunkX == cx && chunk.ChunkZ == cz)
				{
					chunk.Instances.push_back(inst);
					chunk.DirtyBuffer = true;
					chunkFound = true;
					break;
				}
			}

			if (!chunkFound)
			{
				FoliageChunk newChunk;
				newChunk.ChunkX = cx;
				newChunk.ChunkZ = cz;
				newChunk.BoundsMin = glm::vec3(static_cast<float>(cx) * chunkSize, -1000.0f, static_cast<float>(cz) * chunkSize);
				newChunk.BoundsMax = glm::vec3(static_cast<float>(cx + 1) * chunkSize, 1000.0f, static_cast<float>(cz + 1) * chunkSize);
				newChunk.Instances.push_back(inst);
				newChunk.DirtyBuffer = true;
				foliageData.GetChunksMut().push_back(std::move(newChunk));
			}

			++spawnedCount;
		}

		return spawnedCount;
	}

	uint32_t PCGBiomeGenerator::ClearFoliageAlongSpline(
		FoliageData& foliageData,
		const Spline3D& spline,
		float clearWidth)
	{
		const float totalLen = spline.GetTotalLength();
		if (totalLen <= 1.0f || clearWidth <= 0.5f)
			return 0;

		const float step = clearWidth * 0.5f;
		const uint32_t numSamples = static_cast<uint32_t>(totalLen / step);
		uint32_t totalErased = 0;

		for (uint32_t i = 0; i <= numSamples; ++i)
		{
			const float d = std::min(static_cast<float>(i) * step, totalLen);
			glm::vec3 splinePos, tangent;
			spline.SampleAtDistance(d, splinePos, tangent);
			totalErased += foliageData.EraseFoliage(splinePos, clearWidth * 0.5f, -1);
		}

		return totalErased;
	}
}
