#include "wldpch.h"
#include "World/Environment/PCGBiome.h"
#include "World/Foliage/Foliage.h"
#include "World/Utils/Paths.h"
#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
	}

	// -------------------------------------------------------------------------
	// PCGAttributeSet
	// -------------------------------------------------------------------------
	void PCGAttributeSet::SetFloat(const std::string& name, float value)
	{
		m_Floats[name] = value;
	}

	float PCGAttributeSet::GetFloat(const std::string& name, float defaultValue) const
	{
		auto it = m_Floats.find(name);
		return (it != m_Floats.end()) ? it->second : defaultValue;
	}

	bool PCGAttributeSet::HasFloat(const std::string& name) const
	{
		return m_Floats.find(name) != m_Floats.end();
	}

	void PCGAttributeSet::SetString(const std::string& name, const std::string& value)
	{
		m_Strings[name] = value;
	}

	std::string PCGAttributeSet::GetString(const std::string& name, const std::string& defaultValue) const
	{
		auto it = m_Strings.find(name);
		return (it != m_Strings.end()) ? it->second : defaultValue;
	}

	bool PCGAttributeSet::HasString(const std::string& name) const
	{
		return m_Strings.find(name) != m_Strings.end();
	}

	void PCGAttributeSet::Clear()
	{
		m_Floats.clear();
		m_Strings.clear();
	}

	// -------------------------------------------------------------------------
	// PCGFilterRange
	// -------------------------------------------------------------------------
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

	// -------------------------------------------------------------------------
	// PCGRuleGraphDesc: 纯净空白设计 (零写死预设)
	// -------------------------------------------------------------------------
	PCGRuleGraphDesc PCGRuleGraphDesc::CreateBlank()
	{
		PCGRuleGraphDesc desc;
		desc.SampleSpacing = 2.5f;
		desc.JitterFactor = 0.85f;
		desc.IsCustom = true;
		desc.Layers.clear(); // 纯净 0 个图层
		return desc;
	}

	std::string PCGRuleGraphDesc::ToYamlString() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "SampleSpacing" << YAML::Value << SampleSpacing;
		out << YAML::Key << "JitterFactor" << YAML::Value << JitterFactor;
		out << YAML::Key << "IsCustom" << YAML::Value << IsCustom;

		out << YAML::Key << "Layers" << YAML::Value << YAML::BeginSeq;
		for (const auto& layer : Layers)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "LayerName" << YAML::Value << layer.LayerName;
			out << YAML::Key << "Enabled" << YAML::Value << layer.Enabled;
			out << YAML::Key << "Priority" << YAML::Value << layer.Priority;
			out << YAML::Key << "ColorTag" << YAML::Value << YAML::Flow << YAML::BeginSeq
				<< layer.ColorTag.r << layer.ColorTag.g << layer.ColorTag.b << layer.ColorTag.a << YAML::EndSeq;

			out << YAML::Key << "ElevationBand" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "Min" << YAML::Value << layer.ElevationBand.Min;
			out << YAML::Key << "Max" << YAML::Value << layer.ElevationBand.Max;
			out << YAML::Key << "Falloff" << YAML::Value << layer.ElevationBand.Falloff;
			out << YAML::EndMap;

			out << YAML::Key << "SlopeDegrees" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "Min" << YAML::Value << layer.SlopeDegrees.Min;
			out << YAML::Key << "Max" << YAML::Value << layer.SlopeDegrees.Max;
			out << YAML::Key << "Falloff" << YAML::Value << layer.SlopeDegrees.Falloff;
			out << YAML::EndMap;

			out << YAML::Key << "DensityMask" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "Min" << YAML::Value << layer.DensityMask.Min;
			out << YAML::Key << "Max" << YAML::Value << layer.DensityMask.Max;
			out << YAML::Key << "Falloff" << YAML::Value << layer.DensityMask.Falloff;
			out << YAML::EndMap;

			out << YAML::Key << "Entries" << YAML::Value << YAML::BeginSeq;
			for (const auto& ent : layer.Entries)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "Name" << YAML::Value << ent.Name;
				out << YAML::Key << "MeshAsset" << YAML::Value << (ent.MeshAsset.HasPath() ? StringPool::Get().PathOf(ent.MeshAsset.Path) : "");
				out << YAML::Key << "MaterialAsset" << YAML::Value << (ent.MaterialAsset.HasPath() ? StringPool::Get().PathOf(ent.MaterialAsset.Path) : "");
				out << YAML::Key << "SelectionWeight" << YAML::Value << ent.SelectionWeight;
				out << YAML::Key << "ScaleMin" << YAML::Value << ent.ScaleRange.x;
				out << YAML::Key << "ScaleMax" << YAML::Value << ent.ScaleRange.y;
				out << YAML::Key << "AlignToNormal" << YAML::Value << ent.AlignToNormal;
				out << YAML::Key << "FootprintRadius" << YAML::Value << ent.FootprintRadius;
				out << YAML::Key << "Stiffness" << YAML::Value << ent.Stiffness;
				out << YAML::Key << "EnablePhysicalInteraction" << YAML::Value << ent.EnablePhysicalInteraction;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;

			out << YAML::EndMap;
		}
		out << YAML::EndSeq;

		out << YAML::EndMap;
		return out.c_str();
	}

	bool PCGRuleGraphDesc::FromYamlString(const std::string& yamlStr, std::string* error)
	{
		try
		{
			YAML::Node root = YAML::Load(yamlStr);
			if (!root.IsMap())
			{
				if (error) *error = "Root YAML node is not a map";
				return false;
			}
			if (root["SampleSpacing"]) SampleSpacing = root["SampleSpacing"].as<float>();
			if (root["JitterFactor"]) JitterFactor = root["JitterFactor"].as<float>();
			if (root["IsCustom"]) IsCustom = root["IsCustom"].as<bool>();

			if (root["Layers"] && root["Layers"].IsSequence())
			{
				Layers.clear();
				for (const auto& lNode : root["Layers"])
				{
					PCGBiomeLayerDesc layer;
					if (lNode["LayerName"]) layer.LayerName = lNode["LayerName"].as<std::string>();
					if (lNode["Enabled"]) layer.Enabled = lNode["Enabled"].as<bool>();
					if (lNode["Priority"]) layer.Priority = lNode["Priority"].as<float>();
					if (lNode["ColorTag"] && lNode["ColorTag"].IsSequence() && lNode["ColorTag"].size() >= 4)
					{
						layer.ColorTag = glm::vec4(
							lNode["ColorTag"][0].as<float>(),
							lNode["ColorTag"][1].as<float>(),
							lNode["ColorTag"][2].as<float>(),
							lNode["ColorTag"][3].as<float>());
					}

					if (lNode["ElevationBand"])
					{
						auto eb = lNode["ElevationBand"];
						if (eb["Min"]) layer.ElevationBand.Min = eb["Min"].as<float>();
						if (eb["Max"]) layer.ElevationBand.Max = eb["Max"].as<float>();
						if (eb["Falloff"]) layer.ElevationBand.Falloff = eb["Falloff"].as<float>();
					}
					if (lNode["SlopeDegrees"])
					{
						auto sb = lNode["SlopeDegrees"];
						if (sb["Min"]) layer.SlopeDegrees.Min = sb["Min"].as<float>();
						if (sb["Max"]) layer.SlopeDegrees.Max = sb["Max"].as<float>();
						if (sb["Falloff"]) layer.SlopeDegrees.Falloff = sb["Falloff"].as<float>();
					}
					if (lNode["DensityMask"] || lNode["MoistureBand"])
					{
						auto mb = lNode["DensityMask"] ? lNode["DensityMask"] : lNode["MoistureBand"];
						if (mb["Min"]) layer.DensityMask.Min = mb["Min"].as<float>();
						if (mb["Max"]) layer.DensityMask.Max = mb["Max"].as<float>();
						if (mb["Falloff"]) layer.DensityMask.Falloff = mb["Falloff"].as<float>();
					}

					if (lNode["Entries"] && lNode["Entries"].IsSequence())
					{
						for (const auto& eNode : lNode["Entries"])
						{
							PCGSpawnEntry entry;
							if (eNode["Name"]) entry.Name = eNode["Name"].as<std::string>();
							if (eNode["MeshAsset"])
							{
								std::string mp = eNode["MeshAsset"].as<std::string>();
								if (!mp.empty()) entry.MeshAsset = AssetRef::FromPath(mp);
							}
							if (eNode["MaterialAsset"])
							{
								std::string matp = eNode["MaterialAsset"].as<std::string>();
								if (!matp.empty()) entry.MaterialAsset = AssetRef::FromPath(matp);
							}
							if (eNode["SelectionWeight"]) entry.SelectionWeight = eNode["SelectionWeight"].as<float>();
							float sMin = 0.85f, sMax = 1.25f;
							if (eNode["ScaleMin"]) sMin = eNode["ScaleMin"].as<float>();
							if (eNode["ScaleMax"]) sMax = eNode["ScaleMax"].as<float>();
							entry.ScaleRange = glm::vec2(sMin, sMax);
							if (eNode["AlignToNormal"]) entry.AlignToNormal = eNode["AlignToNormal"].as<float>();
							if (eNode["FootprintRadius"]) entry.FootprintRadius = eNode["FootprintRadius"].as<float>();
							if (eNode["Stiffness"]) entry.Stiffness = eNode["Stiffness"].as<float>();
							if (eNode["EnablePhysicalInteraction"]) entry.EnablePhysicalInteraction = eNode["EnablePhysicalInteraction"].as<bool>();
							layer.Entries.push_back(entry);
						}
					}
					Layers.push_back(layer);
				}
			}
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	bool PCGRuleGraphDesc::SaveToFile(const std::string& physicalPath, std::string* error) const
	{
		std::filesystem::path p(physicalPath);
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);

		std::ofstream ofs(physicalPath);
		if (!ofs.is_open())
		{
			if (error) *error = "Failed to open file for write: " + physicalPath;
			return false;
		}
		ofs << ToYamlString();
		return true;
	}

	bool PCGRuleGraphDesc::LoadFromFile(const std::string& physicalPath, std::string* error)
	{
		std::ifstream ifs(physicalPath);
		if (!ifs.is_open())
		{
			if (error) *error = "Failed to open file for read: " + physicalPath;
			return false;
		}
		std::stringstream ss;
		ss << ifs.rdbuf();
		return FromYamlString(ss.str(), error);
	}

	// -------------------------------------------------------------------------
	// PCGGraphAsset
	// -------------------------------------------------------------------------
	std::unordered_map<std::string, std::shared_ptr<PCGGraphAsset>> PCGGraphAsset::s_AssetCache;

	std::string PCGGraphAsset::ToYamlString() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "FormatVersion" << YAML::Value << Version;
		out << YAML::Key << "AssetType" << YAML::Value << "PCGGraph";
		out << YAML::Key << "Name" << YAML::Value << Name;

		YAML::Node graphNode = YAML::Load(RuleGraph.ToYamlString());
		out << YAML::Key << "RuleGraph" << YAML::Value << graphNode;

		out << YAML::EndMap;
		return out.c_str();
	}

	bool PCGGraphAsset::FromYamlString(const std::string& yamlStr, std::string* error)
	{
		try
		{
			YAML::Node root = YAML::Load(yamlStr);
			if (!root.IsMap())
			{
				if (error) *error = "Root node is not a map";
				return false;
			}
			if (root["FormatVersion"]) Version = root["FormatVersion"].as<uint32_t>();
			if (root["Name"]) Name = root["Name"].as<std::string>();
			if (root["RuleGraph"])
			{
				YAML::Emitter emitter;
				emitter << root["RuleGraph"];
				return RuleGraph.FromYamlString(emitter.c_str(), error);
			}
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	bool PCGGraphAsset::SaveToFile(const std::string& physicalPath, std::string* error) const
	{
		std::filesystem::path p(physicalPath);
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);

		std::ofstream ofs(physicalPath);
		if (!ofs.is_open())
		{
			if (error) *error = "Failed to open file for write: " + physicalPath;
			return false;
		}
		ofs << ToYamlString();
		return true;
	}

	bool PCGGraphAsset::LoadFromFile(const std::string& physicalPath, std::string* error)
	{
		std::ifstream ifs(physicalPath);
		if (!ifs.is_open())
		{
			if (error) *error = "Failed to open file for read: " + physicalPath;
			return false;
		}
		std::stringstream ss;
		ss << ifs.rdbuf();
		return FromYamlString(ss.str(), error);
	}

	std::shared_ptr<PCGGraphAsset> PCGGraphAsset::GetOrLoad(const std::string& logicalOrPhysicalPath)
	{
		if (logicalOrPhysicalPath.empty()) return nullptr;

		auto it = s_AssetCache.find(logicalOrPhysicalPath);
		if (it != s_AssetCache.end()) return it->second;

		std::filesystem::path resolved = logicalOrPhysicalPath;
		if (!std::filesystem::exists(resolved))
			resolved = Paths::AssetRoot() / logicalOrPhysicalPath;
		if (!std::filesystem::exists(resolved))
			resolved = Paths::ProjectDir() / logicalOrPhysicalPath;

		if (std::filesystem::exists(resolved))
		{
			auto asset = std::make_shared<PCGGraphAsset>();
			if (asset->LoadFromFile(resolved.string()))
			{
				s_AssetCache[logicalOrPhysicalPath] = asset;
				return asset;
			}
		}
		return nullptr;
	}

	void PCGGraphAsset::ClearCache()
	{
		s_AssetCache.clear();
	}

	// -------------------------------------------------------------------------
	// PCGBiomeGenerator
	// -------------------------------------------------------------------------
	namespace
	{
		PCGRuleGraphDesc s_ActiveRuleGraph = PCGRuleGraphDesc::CreateBlank();
		bool s_RuleGraphInitialized = false;
		std::vector<PCGSpawnPoint> s_SharedPreviewPoints;
		bool s_SharedPreviewActive = false;
	}

	PCGRuleGraphDesc& PCGBiomeGenerator::GetMutableRuleGraph()
	{
		if (!s_RuleGraphInitialized)
		{
			s_ActiveRuleGraph = PCGRuleGraphDesc::CreateBlank();
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
		(void)comp;
		s_ActiveRuleGraph = PCGRuleGraphDesc::CreateBlank();
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
		float roadClearWidth,
		const PCGRuleGraphDesc* customGraph)
	{
		PCGPointData pointData;
		GeneratePointData(terrain, comp, pointData, terrainWorld, roadSplines, roadClearWidth, customGraph);
		outPoints.clear();
		outPoints.reserve(pointData.Points.size());
		for (const auto& pt : pointData.Points)
		{
			PCGSpawnPoint sp;
			sp.Position = pt.Position;
			sp.Normal = pt.Normal;
			sp.LayerId = pt.LayerIndex;
			sp.Weight = pt.Density;
			sp.ColorTag = pt.ColorTag;
			sp.MeshPath = pt.MeshPath;
			sp.MaterialPath = pt.MaterialPath;
			sp.Scale = pt.Scale.x;
			outPoints.push_back(sp);
		}
	}

	void PCGBiomeGenerator::GeneratePointData(
		const TerrainData& terrain,
		const BiomePCGComponent& comp,
		PCGPointData& outData,
		const glm::mat4& terrainWorld,
		const std::vector<Spline3D>* roadSplines,
		float roadClearWidth,
		const PCGRuleGraphDesc* customGraph)
	{
		outData.Clear();
		outData.WorldTransform = terrainWorld;

		const float w = terrain.GetWorldWidth();
		const float h = terrain.GetWorldHeight();
		if (w <= 0.0f || h <= 0.0f)
			return;

		PCGRuleGraphDesc resolvedGraph;
		if (customGraph)
		{
			resolvedGraph = *customGraph;
		}
		else if (comp.RuleAsset.HasPath())
		{
			std::string pathStr = StringPool::Get().PathOf(comp.RuleAsset.Path);
			auto asset = PCGGraphAsset::GetOrLoad(pathStr);
			if (asset)
			{
				resolvedGraph = asset->RuleGraph;
			}
			else
			{
				resolvedGraph = GetActiveRuleGraph();
			}
		}
		else
		{
			resolvedGraph = GetActiveRuleGraph();
		}

		// 根据全局密度倍数计算采样步长
		const float densityMult = std::clamp(comp.DensityMultiplier, 0.05f, 10.0f);
		const float baseSpacing = std::max(0.5f, resolvedGraph.SampleSpacing);
		const float spacing = std::max(0.2f, baseSpacing / std::sqrt(densityMult));

		// 样条避让通道计算
		struct RoadSegment2D { glm::vec2 p0; glm::vec2 p1; };
		std::vector<RoadSegment2D> roadSegments;
		const float totalClearWidth = roadClearWidth + comp.ClearanceMargin;
		const float halfClearWidth = totalClearWidth * 0.5f;
		const float clearWidthSq = halfClearWidth * halfClearWidth;

		if (roadSplines && !roadSplines->empty() && totalClearWidth > 0.0f)
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

		uint32_t pointIndexCounter = 0;
		for (uint32_t z = 0; z < stepsZ; ++z)
		{
			for (uint32_t x = 0; x < stepsX; ++x)
			{
				// 伪随机空间抖动
				float hash1 = std::sin(static_cast<float>(x) * 12.9898f + static_cast<float>(z) * 78.233f + static_cast<float>(comp.Seed)) * 43758.5453f;
				hash1 = hash1 - std::floor(hash1);
				float hash2 = std::sin(static_cast<float>(x) * 39.346f + static_cast<float>(z) * 11.135f + static_cast<float>(comp.Seed)) * 43758.5453f;
				hash2 = hash2 - std::floor(hash2);

				const float jitterX = (hash1 - 0.5f) * spacing * resolvedGraph.JitterFactor;
				const float jitterZ = (hash2 - 0.5f) * spacing * resolvedGraph.JitterFactor;

				const float lx = std::clamp(static_cast<float>(x) * spacing + jitterX, 0.0f, w);
				const float lz = std::clamp(static_cast<float>(z) * spacing + jitterZ, 0.0f, h);

				const float elev = terrain.SampleHeight(lx, lz);
				const glm::vec3 localNormal = terrain.SampleNormal(lx, lz);
				const float slopeDeg = std::acos(std::clamp(localNormal.y, 0.0f, 1.0f)) * (180.0f / kPi);

				// 分形噪声密度
				const float n1 = std::sin(lx * 0.035f + static_cast<float>(comp.Seed)) * std::cos(lz * 0.035f);
				const float n2 = std::sin(lx * 0.085f + static_cast<float>(comp.Seed * 3)) * std::cos(lz * 0.085f) * 0.4f;
				const float densityVal = std::clamp(0.5f + 0.5f * (n1 + n2) / 1.4f, 0.0f, 1.0f);

				glm::vec3 worldPos = glm::vec3(terrainWorld * glm::vec4(lx, elev, lz, 1.0f));

				// 样条避让检测
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
						if (glm::dot(diff, diff) <= clearWidthSq)
						{
							inRoad = true;
							break;
						}
					}
					if (inRoad)
						continue;
				}

				// 图层匹配评估
				uint32_t chosenLayerIdx = 0;
				glm::vec4 pointColor = glm::vec4(0.95f, 0.85f, 0.35f, 0.90f); // 默认纯净金黄点
				std::string assignedMesh;
				std::string assignedMat;
				float randScale = 0.85f + 0.4f * hash2;
				float alignNorm = 0.25f;

				if (!resolvedGraph.Layers.empty())
				{
					float bestScore = -1.0f;
					int bestIdx = -1;
					for (size_t li = 0; li < resolvedGraph.Layers.size(); ++li)
					{
						const auto& layer = resolvedGraph.Layers[li];
						if (!layer.Enabled) continue;
						float wElev = layer.ElevationBand.EvaluateWeight(elev);
						float wSlope = layer.SlopeDegrees.EvaluateWeight(slopeDeg);
						float wDensity = layer.DensityMask.EvaluateWeight(densityVal);
						float score = wElev * wSlope * wDensity * layer.Priority;
						if (score > bestScore && score > 0.01f)
						{
							bestScore = score;
							bestIdx = static_cast<int>(li);
						}
					}

					// 若配置了图层但该点没有任何图层匹配，则不生成
					if (bestIdx < 0)
						continue;

					chosenLayerIdx = static_cast<uint32_t>(bestIdx);
					const auto& winLayer = resolvedGraph.Layers[bestIdx];
					pointColor = winLayer.ColorTag;

					// 物种资产轮盘挑选 (纯数据驱动，完全遵循用户配置的 Entries)
					if (!winLayer.Entries.empty())
					{
						float totalWeight = 0.0f;
						for (const auto& e : winLayer.Entries) totalWeight += std::max(0.01f, e.SelectionWeight);
						float pick = hash2 * totalWeight;
						float acc = 0.0f;
						const PCGSpawnEntry* pickedEntry = &winLayer.Entries[0];
						for (const auto& e : winLayer.Entries)
						{
							acc += std::max(0.01f, e.SelectionWeight);
							if (pick <= acc)
							{
								pickedEntry = &e;
								break;
							}
						}
						if (pickedEntry->MeshAsset.HasPath())
							assignedMesh = StringPool::Get().PathOf(pickedEntry->MeshAsset.Path);
						if (pickedEntry->MaterialAsset.HasPath())
							assignedMat = StringPool::Get().PathOf(pickedEntry->MaterialAsset.Path);

						randScale = pickedEntry->ScaleRange.x + (pickedEntry->ScaleRange.y - pickedEntry->ScaleRange.x) * hash1;
						alignNorm = pickedEntry->AlignToNormal;
					}
				}

				const glm::vec3 worldNormal = glm::normalize(glm::mat3(terrainWorld) * localNormal);

				PCGPoint pt;
				pt.Position = worldPos;
				pt.Normal = worldNormal;
				pt.Density = densityVal;
				pt.Steepness = slopeDeg;
				pt.Seed = hash1;
				pt.PointId = pointIndexCounter++;
				pt.LayerIndex = chosenLayerIdx;
				pt.ColorTag = pointColor;
				pt.MeshPath = assignedMesh;
				pt.MaterialPath = assignedMat;
				pt.Scale = glm::vec3(randScale);
				pt.BoundsMin = glm::vec3(-0.5f * randScale);
				pt.BoundsMax = glm::vec3(0.5f * randScale);

				float yaw = hash1 * 6.2831853f;
				glm::vec3 up(0.0f, 1.0f, 0.0f);
				glm::vec3 plantNormal = glm::normalize(glm::mix(up, worldNormal, alignNorm));
				glm::quat qYaw = glm::angleAxis(yaw, plantNormal);
				if (glm::length(plantNormal - up) > 1e-4f && glm::length(plantNormal + up) > 1e-4f)
				{
					glm::vec3 axis = glm::normalize(glm::cross(up, plantNormal));
					float angle = std::acos(std::clamp(glm::dot(up, plantNormal), -1.0f, 1.0f));
					pt.Rotation = glm::angleAxis(angle, axis) * qYaw;
				}
				else
				{
					pt.Rotation = qYaw;
				}

				outData.Points.push_back(pt);
			}
		}

		outData.GlobalAttributes.SetFloat("TerrainWorldWidth", w);
		outData.GlobalAttributes.SetFloat("TerrainWorldHeight", h);
		outData.GlobalAttributes.SetFloat("SampleSpacing", spacing);
		outData.GlobalAttributes.SetFloat("MinElevation", minH);
		outData.GlobalAttributes.SetFloat("MaxElevation", maxH);
		outData.GlobalAttributes.SetFloat("GeneratedPointCount", static_cast<float>(outData.Points.size()));
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
					terrain.SetHeight(static_cast<uint32_t>(gx), static_cast<uint32_t>(gz), targetY);
				}
				else
				{
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

		if (foliageData.GetTotalInstanceCount() > 0 && points.size() > 16)
		{
			if (preserveHandPainted)
				foliageData.ClearPCGInstances();
			else
				foliageData.Clear();
		}

		// 确保点云与规则图中引用的自定义 Mesh 资产在 FoliageData 中登记为专属 FoliageType
		std::unordered_map<std::string, size_t> meshToTypeIndex;
		for (size_t ti = 0; ti < foliageData.GetTypeCount(); ++ti)
		{
			const auto& ft = foliageData.GetType(ti);
			if (ft.MeshAsset.HasPath())
				meshToTypeIndex[StringPool::Get().PathOf(ft.MeshAsset.Path)] = ti;
		}

		const auto& activeGraph = GetActiveRuleGraph();
		for (const auto& layer : activeGraph.Layers)
		{
			for (const auto& ent : layer.Entries)
			{
				if (ent.MeshAsset.HasPath())
				{
					const std::string mp = StringPool::Get().PathOf(ent.MeshAsset.Path);
					if (meshToTypeIndex.find(mp) == meshToTypeIndex.end())
					{
						FoliageType newType;
						newType.Name = ent.Name;
						newType.MeshAsset = ent.MeshAsset;
						newType.MaterialAsset = ent.MaterialAsset;
						newType.ScaleMinMax = ent.ScaleRange;
						newType.AlignToNormal = (ent.AlignToNormal > 0.4f);
						newType.BendingStiffness = ent.Stiffness;
						newType.EnablePhysicalInteraction = ent.EnablePhysicalInteraction;
						newType.PaintingActive = true;
						size_t idx = foliageData.AddType(newType);
						meshToTypeIndex[mp] = idx;
					}
				}
			}
		}

		for (const auto& pt : points)
		{
			if (!pt.MeshPath.empty() && meshToTypeIndex.find(pt.MeshPath) == meshToTypeIndex.end())
			{
				FoliageType newType;
				newType.Name = "PCG Auto Type";
				newType.MeshAsset = AssetRef::FromPath(pt.MeshPath);
				if (!pt.MaterialPath.empty())
					newType.MaterialAsset = AssetRef::FromPath(pt.MaterialPath);
				newType.PaintingActive = true;
				size_t idx = foliageData.AddType(newType);
				meshToTypeIndex[pt.MeshPath] = idx;
			}
		}

		const size_t typeCount = foliageData.GetTypeCount();
		if (typeCount == 0)
			return 0;

		uint32_t spawnedCount = 0;
		std::mt19937 rng(1337);
		std::uniform_real_distribution<float> angleDist(0.0f, 6.2831853f);
		std::uniform_real_distribution<float> scaleDist(0.85f, 1.25f);

		for (const auto& pt : points)
		{
			size_t chosenType = 0;
			if (!pt.MeshPath.empty() && meshToTypeIndex.find(pt.MeshPath) != meshToTypeIndex.end())
			{
				chosenType = meshToTypeIndex[pt.MeshPath];
			}
			else
			{
				chosenType = (pt.LayerId < typeCount) ? pt.LayerId : 0;
			}
			const float s = scale * (pt.Scale > 0.05f ? pt.Scale : scaleDist(rng));
			const float yaw = angleDist(rng);

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
