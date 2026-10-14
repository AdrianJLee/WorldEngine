#include "SoundEvent.h"
#include <algorithm>
#include <random>
#include <sstream>

namespace World::Audio
{
	static std::mt19937 s_Rng(std::random_device{}());

	std::string SoundContainerNode::EvaluateSelection(
		const std::unordered_map<std::string, std::string>& switchParams,
		const std::unordered_map<std::string, float>& blendParams)
	{
		switch (type)
		{
		case ContainerType::Single:
			if (!variations.empty()) return variations[0].soundAssetPath;
			break;

		case ContainerType::Random:
			if (variations.empty()) return "";
			if (variations.size() == 1)
			{
				lastChosenIndex = 0;
				return variations[0].soundAssetPath;
			}
			{
				const bool skipLast = (shuffleNoRepeat && variations.size() > 1 && lastChosenIndex < variations.size());
				float totalWeight = 0.0f;
				for (size_t i = 0; i < variations.size(); ++i)
				{
					if (skipLast && i == lastChosenIndex) continue;
					totalWeight += std::max(0.01f, variations[i].weight);
				}

				if (totalWeight <= 0.0f)
				{
					lastChosenIndex = 0;
					return variations[0].soundAssetPath;
				}

				std::uniform_real_distribution<float> dist(0.0f, totalWeight);
				float roll = dist(s_Rng);

				size_t selectedIndex = static_cast<size_t>(-1);
				size_t lastCandidate = 0;
				float accumulated = 0.0f;
				for (size_t i = 0; i < variations.size(); ++i)
				{
					if (skipLast && i == lastChosenIndex) continue;
					lastCandidate = i;
					accumulated += std::max(0.01f, variations[i].weight);
					if (roll <= accumulated)
					{
						selectedIndex = i;
						break;
					}
				}

				if (selectedIndex == static_cast<size_t>(-1))
				{
					selectedIndex = lastCandidate;
				}

				lastChosenIndex = static_cast<uint32_t>(selectedIndex);
				return variations[selectedIndex].soundAssetPath;
			}

		case ContainerType::Sequence:
			if (!variations.empty())
			{
				uint32_t idx = sequenceIndex % variations.size();
				++sequenceIndex;
				return variations[idx].soundAssetPath;
			}
			break;

		case ContainerType::Switch:
			{
				auto it = switchParams.find(switchParamName);
				if (it != switchParams.end())
				{
					auto branchIt = switchBranches.find(it->second);
					if (branchIt != switchBranches.end() && branchIt->second)
					{
						return branchIt->second->EvaluateSelection(switchParams, blendParams);
					}
				}
				// Default branch fallback if available
				auto defIt = switchBranches.find("Default");
				if (defIt != switchBranches.end() && defIt->second)
				{
					return defIt->second->EvaluateSelection(switchParams, blendParams);
				}
			}
			break;

		case ContainerType::Blend:
			if (!blendPoints.empty())
			{
				float paramVal = 0.0f;
				auto it = blendParams.find(blendParamName);
				if (it != blendParams.end()) paramVal = it->second;

				// Find closest point
				size_t closestIdx = 0;
				float closestDiff = 1e9f;
				for (size_t i = 0; i < blendPoints.size(); ++i)
				{
					float diff = std::abs(blendPoints[i].first - paramVal);
					if (diff < closestDiff)
					{
						closestDiff = diff;
						closestIdx = i;
					}
				}
				if (blendPoints[closestIdx].second)
				{
					return blendPoints[closestIdx].second->EvaluateSelection(switchParams, blendParams);
				}
			}
			break;
		}

		return variations.empty() ? "" : variations[0].soundAssetPath;
	}

	SoundEvent::SoundEvent(std::string id, BusId busId)
		: m_Id(std::move(id)), m_BusId(busId)
	{
		m_RootContainer = std::make_shared<SoundContainerNode>();
		m_RootContainer->type = ContainerType::Single;
	}

	float SoundEvent::EvaluateRandomVolume() const noexcept
	{
		if (m_MinVolume >= m_MaxVolume) return m_MinVolume;
		std::uniform_real_distribution<float> dist(m_MinVolume, m_MaxVolume);
		return dist(s_Rng);
	}

	float SoundEvent::EvaluateRandomPitch() const noexcept
	{
		float semitones = 0.0f;
		if (m_MinPitchSemitones < m_MaxPitchSemitones)
		{
			std::uniform_real_distribution<float> dist(m_MinPitchSemitones, m_MaxPitchSemitones);
			semitones = dist(s_Rng);
		}
		else
		{
			semitones = m_MinPitchSemitones;
		}
		return PitchSemitonesToRatio(semitones);
	}

	std::string SoundEvent::SerializeYaml() const
	{
		std::ostringstream ss;
		ss << "Id: \"" << m_Id << "\"\n";
		ss << "BusId: " << m_BusId << "\n";
		ss << "MaxInstances: " << m_MaxInstances << "\n";
		ss << "CooldownSeconds: " << m_CooldownSeconds << "\n";
		ss << "VolumeRange: [" << m_MinVolume << ", " << m_MaxVolume << "]\n";
		ss << "PitchRange: [" << m_MinPitchSemitones << ", " << m_MaxPitchSemitones << "]\n";
		ss << "Spatial:\n";
		ss << "  Is3D: " << (m_Spatial.is3D ? "true" : "false") << "\n";
		ss << "  SpatialMode: " << static_cast<int>(m_Spatial.mode) << "\n";
		ss << "  HalfViewWidth: " << m_Spatial.halfViewWidth << "\n";
		ss << "  MaxPan: " << m_Spatial.maxPan << "\n";
		ss << "  MinDistance: " << m_Spatial.minDistance << "\n";
		ss << "  MaxDistance: " << m_Spatial.maxDistance << "\n";
		ss << "  AttenuationModel: " << static_cast<int>(m_Spatial.model) << "\n";

		if (m_RootContainer)
		{
			ss << "RootContainer:\n";
			ss << "  Type: " << static_cast<int>(m_RootContainer->type) << "\n";
			ss << "  ShuffleNoRepeat: " << (m_RootContainer->shuffleNoRepeat ? "true" : "false") << "\n";
			ss << "  Variations:\n";
			for (const auto& v : m_RootContainer->variations)
			{
				ss << "    - Path: \"" << v.soundAssetPath << "\"\n";
				ss << "      Weight: " << v.weight << "\n";
			}
		}
		return ss.str();
	}

	bool SoundEvent::DeserializeYaml(const std::string& yamlText)
	{
		// Lightweight parser for standard fields
		std::istringstream stream(yamlText);
		std::string line;
		while (std::getline(stream, line))
		{
			auto colonPos = line.find(':');
			if (colonPos == std::string::npos) continue;

			std::string key = line.substr(0, colonPos);
			// trim key
			while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
			if (!key.empty() && key.front() == '-')
			{
				key.erase(key.begin());
				while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
			}

			if (key == "Id")
			{
				auto q1 = line.find('"', colonPos);
				auto q2 = line.rfind('"');
				if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1)
				{
					m_Id = line.substr(q1 + 1, q2 - q1 - 1);
				}
			}
			else if (key == "BusId")
			{
				m_BusId = static_cast<BusId>(std::stoul(line.substr(colonPos + 1)));
			}
			else if (key == "MaxInstances")
			{
				m_MaxInstances = static_cast<uint32_t>(std::stoul(line.substr(colonPos + 1)));
			}
			else if (key == "Is3D")
			{
				m_Spatial.is3D = (line.find("true") != std::string::npos);
				if (m_Spatial.is3D && m_Spatial.mode == SpatialMode::None) m_Spatial.mode = SpatialMode::Spatial3D;
			}
			else if (key == "SpatialMode")
			{
				m_Spatial.mode = static_cast<SpatialMode>(std::stoul(line.substr(colonPos + 1)));
				if (m_Spatial.mode == SpatialMode::Spatial3D) m_Spatial.is3D = true;
			}
			else if (key == "HalfViewWidth")
			{
				m_Spatial.halfViewWidth = std::stof(line.substr(colonPos + 1));
			}
			else if (key == "MaxPan")
			{
				m_Spatial.maxPan = std::stof(line.substr(colonPos + 1));
			}
			else if (key == "MinDistance")
			{
				m_Spatial.minDistance = std::stof(line.substr(colonPos + 1));
			}
			else if (key == "MaxDistance")
			{
				m_Spatial.maxDistance = std::stof(line.substr(colonPos + 1));
			}
			else if (key == "Path")
			{
				auto q1 = line.find('"', colonPos);
				auto q2 = line.rfind('"');
				if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1)
				{
					if (!m_RootContainer) m_RootContainer = std::make_shared<SoundContainerNode>();
					SoundVariation v;
					v.soundAssetPath = line.substr(q1 + 1, q2 - q1 - 1);
					m_RootContainer->variations.push_back(v);
				}
			}
			else if (key == "Weight")
			{
				if (m_RootContainer && !m_RootContainer->variations.empty())
				{
					try
					{
						m_RootContainer->variations.back().weight = std::stof(line.substr(colonPos + 1));
					}
					catch (...)
					{
						m_RootContainer->variations.back().weight = 1.0f;
					}
				}
			}
		}
		return true;
	}
}
