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

	float PCGFilterRange::EvaluateWeight(float value) const
	{
		if (value >= Min && value <= Max)
			return 1.0f;

		if (Falloff <= 1e-4f)
			return 0.0f;

		if (value < Min)
		{
			if (value <= Min - Falloff)
				return 0.0f;
			return std::clamp((value - (Min - Falloff)) / Falloff, 0.0f, 1.0f);
		}
		else
		{
			if (value >= Max + Falloff)
				return 0.0f;
			return std::clamp(((Max + Falloff) - value) / Falloff, 0.0f, 1.0f);
		}
	}

	PCGRuleGraphDesc PCGRuleGraphDesc::CreateAdaptiveDefault(const BiomePCGComponent& comp, float terrainMinY, float terrainMaxY)
	{
		(void)terrainMinY;
		(void)terrainMaxY;
		PCGRuleGraphDesc desc;
		desc.SampleSpacing = 2.5f;
		desc.JitterFactor = 0.85f;
		desc.UseNormalizedElevation = true;
		desc.RockSlopeThresholdDeg = comp.MaxSlope;
		desc.AridMoistureThreshold = std::max(0.25f, comp.MoistureThreshold);
		desc.SnowLineElevation = 150.0f;
		desc.SnowLineNormalized = 0.78f;

		// 1. Meadow
		PCGBiomeLayerDesc meadow;
		meadow.LayerName = "Meadow";
		meadow.Category = BiomeType::Meadow;
		meadow.ElevationBand = { 0.0f, 0.70f, 0.08f };
		meadow.SlopeDegrees = { 0.0f, 25.0f, 5.0f };
		meadow.MoistureBand = { 0.30f, 0.65f, 0.08f };
		meadow.Priority = 1.0f;
		{
			PCGSpawnEntry grass;
			grass.Name = "Wild Grass";
			grass.MeshAsset = AssetRef::FromPath("models/foliage/grass_cluster.wmodel");
			grass.MaterialAsset = AssetRef::FromPath("textures/foliage/grass_tuft.png");
			grass.SelectionWeight = 1.0f;
			grass.ScaleRange = { 0.85f, 1.25f };
			grass.Stiffness = 0.25f;
			grass.EnablePhysicalInteraction = true;
			meadow.Entries.push_back(grass);

			PCGSpawnEntry flower;
			flower.Name = "Wild Flowers";
			flower.MeshAsset = AssetRef::FromPath("models/foliage/grass_cluster.wmodel");
			flower.MaterialAsset = AssetRef::FromPath("textures/foliage/wild_flower.png");
			flower.SelectionWeight = 0.8f;
			flower.ScaleRange = { 0.85f, 1.15f };
			flower.Stiffness = 0.30f;
			flower.EnablePhysicalInteraction = true;
			meadow.Entries.push_back(flower);
		}
		desc.Layers.push_back(meadow);

		// 2. Forest
		PCGBiomeLayerDesc forest;
		forest.LayerName = "Forest";
		forest.Category = BiomeType::Forest;
		forest.ElevationBand = { 0.05f, 0.72f, 0.08f };
		forest.SlopeDegrees = { 0.0f, 32.0f, 5.0f };
		forest.MoistureBand = { 0.50f, 1.0f, 0.08f };
		forest.Priority = 1.2f;
		{
			PCGSpawnEntry shrub;
			shrub.Name = "Forest Shrub";
			shrub.MeshAsset = AssetRef::FromPath("models/foliage/bush_cluster.wmodel");
			shrub.MaterialAsset = AssetRef::FromPath("textures/foliage/bush_leaves.png");
			shrub.SelectionWeight = 1.0f;
			shrub.ScaleRange = { 0.9f, 1.35f };
			shrub.Stiffness = 0.55f;
			shrub.EnablePhysicalInteraction = true;
			forest.Entries.push_back(shrub);
		}
		desc.Layers.push_back(forest);

		// 3. Rocky Mountain
		PCGBiomeLayerDesc rocky;
		rocky.LayerName = "RockyMountain";
		rocky.Category = BiomeType::RockyMountain;
		rocky.ElevationBand = { 0.20f, 1.0f, 0.08f };
		rocky.SlopeDegrees = { desc.RockSlopeThresholdDeg, 78.0f, 5.0f };
		rocky.MoistureBand = { 0.0f, 1.0f, 0.08f };
		rocky.Priority = 1.5f;
		{
			PCGSpawnEntry rock;
			rock.Name = "Scatter Rocks";
			rock.MeshAsset = AssetRef::FromPath("models/foliage/scatter_pebble.wmodel");
			rock.MaterialAsset = AssetRef::FromPath("textures/terrain/rock_albedo.png");
			rock.SelectionWeight = 1.0f;
			rock.ScaleRange = { 0.7f, 1.4f };
			rock.Stiffness = 10.0f;
			rock.EnablePhysicalInteraction = false;
			rocky.Entries.push_back(rock);
		}
		desc.Layers.push_back(rocky);

		// 4. Desert
		PCGBiomeLayerDesc desert;
		desert.LayerName = "Desert";
		desert.Category = BiomeType::Desert;
		desert.ElevationBand = { 0.0f, 0.80f, 0.08f };
		desert.SlopeDegrees = { 0.0f, 35.0f, 5.0f };
		desert.MoistureBand = { 0.0f, desc.AridMoistureThreshold, 0.05f };
		desert.Priority = 1.4f;
		{
			PCGSpawnEntry rock;
			rock.Name = "Arid Rocks";
			rock.MeshAsset = AssetRef::FromPath("models/foliage/scatter_pebble.wmodel");
			rock.MaterialAsset = AssetRef::FromPath("textures/terrain/rock_albedo.png");
			rock.SelectionWeight = 1.0f;
			rock.ScaleRange = { 0.75f, 1.25f };
			rock.Stiffness = 10.0f;
			rock.EnablePhysicalInteraction = false;
			desert.Entries.push_back(rock);
		}
		desc.Layers.push_back(desert);

		// 5. Snow Peak
		PCGBiomeLayerDesc snow;
		snow.LayerName = "SnowPeak";
		snow.Category = BiomeType::SnowPeak;
		snow.ElevationBand = { desc.SnowLineNormalized, 1.0f, 0.05f };
		snow.SlopeDegrees = { 0.0f, 65.0f, 5.0f };
		snow.MoistureBand = { 0.0f, 1.0f, 0.08f };
		snow.Priority = 2.0f;
		{
			PCGSpawnEntry snowRock;
			snowRock.Name = "Glacial Pebble";
			snowRock.MeshAsset = AssetRef::FromPath("models/foliage/scatter_pebble.wmodel");
			snowRock.MaterialAsset = AssetRef::FromPath("textures/terrain/rock_albedo.png");
			snowRock.SelectionWeight = 1.0f;
			snowRock.ScaleRange = { 0.8f, 1.5f };
			snowRock.Stiffness = 10.0f;
			snowRock.EnablePhysicalInteraction = false;
			snow.Entries.push_back(snowRock);
		}
		desc.Layers.push_back(snow);

		return desc;
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

	BiomeType PCGBiomeGenerator::ClassifyBiomeAdaptive(
		float elevation,
		float normalizedElev,
		float slopeDeg,
		float moisture,
		const BiomePCGComponent& comp)
	{
		if ((elevation > 150.0f || normalizedElev >= 0.65f) && slopeDeg <= 60.0f)
			return BiomeType::SnowPeak;

		if (slopeDeg > comp.MaxSlope)
			return BiomeType::RockyMountain;

		if (moisture < std::max(0.25f, comp.MoistureThreshold))
			return BiomeType::Desert;

		if (moisture >= 0.5f && normalizedElev < 0.72f)
			return BiomeType::Forest;

		return BiomeType::Meadow;
	}


	namespace
	{
		PCGRuleGraphDesc s_ActiveRuleGraph;
		bool s_RuleGraphInitialized = false;
		std::vector<PCGSpawnPoint> s_SharedPreviewPoints;
		bool s_SharedPreviewActive = false;
	}

	PCGRuleGraphDesc& PCGBiomeGenerator::GetMutableRuleGraph()
	{
		if (!s_RuleGraphInitialized)
		{
			BiomePCGComponent defaultComp;
			s_ActiveRuleGraph = PCGRuleGraphDesc::CreateAdaptiveDefault(defaultComp);
			s_RuleGraphInitialized = true;
		}
		return s_ActiveRuleGraph;
	}

	const PCGRuleGraphDesc& PCGBiomeGenerator::GetActiveRuleGraph()
	{
		return GetMutableRuleGraph();
	}

	void PCGBiomeGenerator::ResetRuleGraphToDefault(const BiomePCGComponent& comp)
	{
		s_ActiveRuleGraph = PCGRuleGraphDesc::CreateAdaptiveDefault(comp);
		s_RuleGraphInitialized = true;
	}

	std::vector<PCGSpawnPoint>& PCGBiomeGenerator::GetSharedPreviewPoints()
	{
		return s_SharedPreviewPoints;
	}

	bool PCGBiomeGenerator::IsSharedPreviewActive()
	{
		return s_SharedPreviewActive;
	}

	void PCGBiomeGenerator::SetSharedPreviewActive(bool active)
	{
		s_SharedPreviewActive = active;
	}
	void PCGBiomeGenerator::GenerateBiomePoints(
		const TerrainData& terrain,
		const BiomePCGComponent& comp,
		std::vector<PCGSpawnPoint>& outPoints,
		const glm::mat4& terrainWorld,
		const std::vector<Spline3D>* roadSplines,
		float roadClearWidth)
	{
		outPoints.clear();
		const float w = terrain.GetWorldWidth();
		const float h = terrain.GetWorldHeight();
		if (w <= 0.0f || h <= 0.0f)
			return;

		// 自然步长 (2.5m 形成细腻繁茂覆盖，杜绝 8m 巨大方格阵)
		const float spacing = std::max(0.5f, GetActiveRuleGraph().SampleSpacing);

		struct RoadSegment2D { glm::vec2 p0; glm::vec2 p1; };
		std::vector<RoadSegment2D> roadSegments;
		const float halfRoadWidth = roadClearWidth * 0.5f;
		const float roadWidthSq = halfRoadWidth * halfRoadWidth;

		if (roadSplines && !roadSplines->empty() && roadClearWidth > 0.0f)
		{
			for (const auto& spline : *roadSplines)
			{
				const float totalLen = spline.GetTotalLength();
				if (totalLen <= 0.1f)
					continue;
				const float step = 1.0f;
				const uint32_t numSteps = std::max(2u, static_cast<uint32_t>(std::ceil(totalLen / step)) + 1u);
				glm::vec3 prevPos(0.0f), tan(0.0f);
				spline.SampleAtDistance(0.0f, prevPos, tan);
				for (uint32_t s = 1; s < numSteps; ++s)
				{
					const float d = std::min(static_cast<float>(s) * step, totalLen);
					glm::vec3 currPos(0.0f);
					spline.SampleAtDistance(d, currPos, tan);
					roadSegments.push_back({ glm::vec2(prevPos.x, prevPos.z), glm::vec2(currPos.x, currPos.z) });
					prevPos = currPos;
				}
			}
		}
		const uint32_t stepsX = std::max(1u, static_cast<uint32_t>(w / spacing));
		const uint32_t stepsZ = std::max(1u, static_cast<uint32_t>(h / spacing));

		// 先扫描地形采样网格求出实际高度范围 [minH, maxH]
		float minH = 1e9f;
		float maxH = -1e9f;
		const uint32_t tw = terrain.GetWidth();
		const uint32_t th = terrain.GetHeight();
		if (tw > 0 && th > 0)
		{
			for (uint32_t gz = 0; gz < th; ++gz)
			{
				for (uint32_t gx = 0; gx < tw; ++gx)
				{
					const float heightVal = terrain.GetHeight(gx, gz);
					minH = std::min(minH, heightVal);
					maxH = std::max(maxH, heightVal);
				}
			}
		}
		else
		{
			minH = 0.0f;
			maxH = 100.0f;
		}

		const float hRange = std::max(0.001f, maxH - minH);

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
				// 绝壁 (>78°) 剔除
				if (slopeDeg > 78.0f)
					continue;

				// 多八度分形湿度计算 (Fractal Moisture Noise)
				const float n1 = std::sin(lx * 0.035f + static_cast<float>(comp.Seed)) * std::cos(lz * 0.035f);
				const float n2 = std::sin(lx * 0.085f + static_cast<float>(comp.Seed * 3)) * std::cos(lz * 0.085f) * 0.4f;
				const float moisture = std::clamp(0.5f + 0.5f * (n1 + n2) / 1.4f, 0.0f, 1.0f);

				// 当低于湿度阈值时，按 45% 概率保留为 Desert (荒漠碎石/耐旱植被)，否则剔除
				if (moisture < comp.MoistureThreshold)
				{
					if (hash2 >= 0.45f)
						continue;
				}

				const float normalizedElev = std::clamp((elev - minH) / hRange, 0.0f, 1.0f);

				glm::vec3 worldPos = glm::vec3(terrainWorld * glm::vec4(lx, elev, lz, 1.0f));
				if (!roadSegments.empty())
				{
					bool inRoad = false;
					const glm::vec2 pXZ(worldPos.x, worldPos.z);
					for (const auto& seg : roadSegments)
					{
						const glm::vec2 ab = seg.p1 - seg.p0;
						const float lenSq = glm::dot(ab, ab);
						float u = 0.0f;
						if (lenSq > 1e-6f)
							u = std::clamp(glm::dot(pXZ - seg.p0, ab) / lenSq, 0.0f, 1.0f);
						const glm::vec2 closest = seg.p0 + u * ab;
						const glm::vec2 diff = pXZ - closest;
						if (glm::dot(diff, diff) <= roadWidthSq)
						{
							inRoad = true;
							break;
						}
					}
					if (inRoad)
						continue;
				}

				PCGSpawnPoint pt;
				pt.Position = worldPos;
				pt.Normal = glm::normalize(glm::mat3(terrainWorld) * localNormal);
				const auto& ruleGraph = GetActiveRuleGraph();
				if (ruleGraph.IsCustom && !ruleGraph.Layers.empty())
				{
					float bestLayerScore = -1.0f;
					BiomeType bestBiome = BiomeType::Meadow;
					for (const auto& layer : ruleGraph.Layers)
					{
						float elevVal = ruleGraph.UseNormalizedElevation ? normalizedElev : elev;
						float wElev = layer.ElevationBand.EvaluateWeight(elevVal);
						float wSlope = layer.SlopeDegrees.EvaluateWeight(slopeDeg);
						float wMoist = layer.MoistureBand.EvaluateWeight(moisture);
						float score = wElev * wSlope * wMoist * layer.Priority;
						if (score > bestLayerScore && score > 0.01f)
						{
							bestLayerScore = score;
							bestBiome = layer.Category;
						}
					}
					if (bestLayerScore > 0.01f)
						pt.Biome = bestBiome;
					else
						pt.Biome = ClassifyBiomeAdaptive(elev, normalizedElev, slopeDeg, moisture, comp);
				}
				else
				{
					pt.Biome = ClassifyBiomeAdaptive(elev, normalizedElev, slopeDeg, moisture, comp);
				}
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

		// 样条高密度采样步长 (0.5m)
		const float sampleStep = 0.5f;
		const uint32_t numSamples = std::max(2u, static_cast<uint32_t>(std::ceil(totalLen / sampleStep)) + 1u);
		const glm::mat4 invTerrain = glm::inverse(terrainWorld);

		std::vector<glm::vec3> localSamples;
		localSamples.reserve(numSamples);

		float minX = 1e9f, maxX = -1e9f;
		float minZ = 1e9f, maxZ = -1e9f;

		for (uint32_t i = 0; i < numSamples; ++i)
		{
			const float d = std::min(static_cast<float>(i) * sampleStep, totalLen);
			glm::vec3 splinePos, tangent;
			spline.SampleAtDistance(d, splinePos, tangent);

			glm::vec3 localPos = glm::vec3(invTerrain * glm::vec4(splinePos, 1.0f));
			localSamples.push_back(localPos);

			minX = std::min(minX, localPos.x);
			maxX = std::max(maxX, localPos.x);
			minZ = std::min(minZ, localPos.z);
			maxZ = std::max(maxZ, localPos.z);
		}

		if (localSamples.size() < 2)
			return;

		const float halfWidth = roadWidth * 0.5f;
		const float totalRadius = halfWidth + slopeTransition;
		const float cellSize = terrain.GetCellSize();
		if (cellSize <= 1e-4f)
			return;

		const int minGX = std::max(0, static_cast<int>(std::floor((minX - totalRadius) / cellSize)));
		const int maxGX = std::min(static_cast<int>(terrain.GetWidth() - 1), static_cast<int>(std::ceil((maxX + totalRadius) / cellSize)));
		const int minGZ = std::max(0, static_cast<int>(std::floor((minZ - totalRadius) / cellSize)));
		const int maxGZ = std::min(static_cast<int>(terrain.GetHeight() - 1), static_cast<int>(std::ceil((maxZ + totalRadius) / cellSize)));

		const size_t segmentCount = localSamples.size() - 1;

		for (int gz = minGZ; gz <= maxGZ; ++gz)
		{
			const float pz = static_cast<float>(gz) * cellSize;
			for (int gx = minGX; gx <= maxGX; ++gx)
			{
				const float px = static_cast<float>(gx) * cellSize;

				// 求点 (px, pz) 到样条折线段序列的最近水平 (XZ) 投影点与距离
				float minDistXZ = 1e9f;
				glm::vec3 closestQ(0.0f);

				for (size_t k = 0; k < segmentCount; ++k)
				{
					const glm::vec3& A = localSamples[k];
					const glm::vec3& B = localSamples[k + 1];

					const glm::vec2 ab(B.x - A.x, B.z - A.z);
					const float lenSq = glm::dot(ab, ab);
					float u = 0.0f;
					if (lenSq > 1e-6f)
					{
						const glm::vec2 ap(px - A.x, pz - A.z);
						u = glm::clamp(glm::dot(ap, ab) / lenSq, 0.0f, 1.0f);
					}

					const glm::vec3 Q = glm::mix(A, B, u);
					const float dx = px - Q.x;
					const float dz = pz - Q.z;
					const float dXZ = std::sqrt(dx * dx + dz * dz);

					if (dXZ < minDistXZ)
					{
						minDistXZ = dXZ;
						closestQ = Q;
					}
				}

				if (minDistXZ > totalRadius)
					continue;

				const float targetY = closestQ.y - cutDepth;

				if (minDistXZ <= halfWidth)
				{
					// 路基核心区：高度绝对平整连续
					terrain.SetHeight(static_cast<uint32_t>(gx), static_cast<uint32_t>(gz), targetY);
				}
				else
				{
					// 边坡过渡区：五次多项式平滑放坡
					const float t = (minDistXZ - halfWidth) / std::max(0.001f, slopeTransition);
					const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
					const float origH = terrain.GetHeight(static_cast<uint32_t>(gx), static_cast<uint32_t>(gz));
					const float newH = glm::mix(targetY, origH, s);
					terrain.SetHeight(static_cast<uint32_t>(gx), static_cast<uint32_t>(gz), newH);
				}
			}
		}

		terrain.MarkDirty(minX - totalRadius, minZ - totalRadius, maxX + totalRadius, maxZ + totalRadius);
	}

	uint32_t PCGBiomeGenerator::PopulateFoliageFromBiomePoints(
		const std::vector<PCGSpawnPoint>& points,
		FoliageData& foliageData,
		float scale,
		bool preserveHandPainted)
	{
		if (points.empty())
			return 0;

		// 幂等清空：若已有实例且大批量重刷 (>16个)，先清空旧实例 (保留类型定义)，防止无限叠加堆叠
		if (foliageData.GetTotalInstanceCount() > 0 && points.size() > 16)
		{
			if (preserveHandPainted)
			{
				foliageData.ClearPCGInstances();
			}
			else
			{
				foliageData.Clear();
			}
		}

		if (foliageData.GetTypeCount() == 0)
		{
			FoliageType grass;
			grass.Name = "0: 翠绿草丛 (Wild Grass)";
			grass.MeshAsset = AssetRef::FromPath("models/foliage/grass_cluster.wmodel");
			grass.MaterialAsset = AssetRef::FromPath("textures/foliage/grass_tuft.png");
			grass.Density = 1.0f;
			grass.BendingStiffness = 0.25f;
			grass.BendingRecovery = 4.5f;
			grass.EnablePhysicalInteraction = true;
			foliageData.AddType(grass);

			FoliageType flower;
			flower.Name = "1: 雏菊野花 (Wild Flowers)";
			flower.MeshAsset = AssetRef::FromPath("models/foliage/grass_cluster.wmodel");
			flower.MaterialAsset = AssetRef::FromPath("textures/foliage/wild_flower.png");
			flower.Density = 0.8f;
			flower.BendingStiffness = 0.30f;
			flower.BendingRecovery = 4.0f;
			flower.EnablePhysicalInteraction = true;
			foliageData.AddType(flower);

			FoliageType shrub;
			shrub.Name = "2: 矮生灌木 (Forest Shrub)";
			shrub.MeshAsset = AssetRef::FromPath("models/foliage/bush_cluster.wmodel");
			shrub.MaterialAsset = AssetRef::FromPath("textures/foliage/bush_leaves.png");
			shrub.Density = 0.5f;
			shrub.BendingStiffness = 0.55f;
			shrub.BendingRecovery = 6.0f;
			shrub.EnablePhysicalInteraction = true;
			foliageData.AddType(shrub);

			FoliageType rock;
			rock.Name = "3: 地表碎石 (Scatter Rocks)";
			rock.MeshAsset = AssetRef::FromPath("models/foliage/scatter_pebble.wmodel");
			rock.MaterialAsset = AssetRef::FromPath("textures/terrain/rock_albedo.png");
			rock.Density = 0.6f;
			rock.BendingStiffness = 10.0f;
			rock.BendingRecovery = 10.0f;
			rock.EnablePhysicalInteraction = false;
			foliageData.AddType(rock);
		}
		else if (foliageData.GetTypeCount() >= 4)
		{
			foliageData.GetType(0).BendingStiffness = 0.25f;
			foliageData.GetType(0).BendingRecovery = 4.5f;
			foliageData.GetType(0).EnablePhysicalInteraction = true;

			foliageData.GetType(1).BendingStiffness = 0.30f;
			foliageData.GetType(1).BendingRecovery = 4.0f;
			foliageData.GetType(1).EnablePhysicalInteraction = true;

			foliageData.GetType(2).BendingStiffness = 0.55f;
			foliageData.GetType(2).BendingRecovery = 6.0f;
			foliageData.GetType(2).EnablePhysicalInteraction = true;

			foliageData.GetType(3).BendingStiffness = 10.0f;
			foliageData.GetType(3).BendingRecovery = 10.0f;
			foliageData.GetType(3).EnablePhysicalInteraction = false;
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
			inst.Params = glm::vec4(angleDist(rng), 1.0f, s, static_cast<float>(chosenType));

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
