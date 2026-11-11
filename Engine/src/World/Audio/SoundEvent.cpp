#include "SoundEvent.h"
#include <algorithm>
#include <random>
#include <sstream>
#include <cmath>

namespace World::Audio
{
	static std::mt19937 s_Rng(std::random_device{}());

	std::string SoundContainerNode::EvaluateSelection(
		const std::unordered_map<std::string, std::string>& switchParams,
		const std::unordered_map<std::string, float>& blendParams)
	{
		auto tracks = EvaluateTracks(switchParams, blendParams);
		if (!tracks.empty())
		{
			return tracks[0].soundAssetPath;
		}
		return variations.empty() ? "" : variations[0].soundAssetPath;
	}

	std::vector<EvaluatedSoundTrack> SoundContainerNode::EvaluateTracks(
		const std::unordered_map<std::string, std::string>& switchParams,
		const std::unordered_map<std::string, float>& blendParams)
	{
		std::vector<EvaluatedSoundTrack> results;

		switch (type)
		{
		case ContainerType::Single:
			if (!variations.empty())
			{
				results.push_back({ variations[0].soundAssetPath, 1.0f, 0.0f });
			}
			break;

		case ContainerType::Random:
			if (!variations.empty())
			{
				if (variations.size() == 1)
				{
					lastChosenIndex = 0;
					results.push_back({ variations[0].soundAssetPath, 1.0f, 0.0f });
				}
				else
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
						results.push_back({ variations[0].soundAssetPath, 1.0f, 0.0f });
					}
					else
					{
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
						results.push_back({ variations[selectedIndex].soundAssetPath, 1.0f, 0.0f });
					}
				}
			}
			break;

		case ContainerType::Sequence:
			if (!variations.empty())
			{
				uint32_t idx = sequenceIndex % variations.size();
				++sequenceIndex;
				results.push_back({ variations[idx].soundAssetPath, 1.0f, 0.0f });
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
						return branchIt->second->EvaluateTracks(switchParams, blendParams);
					}
				}
				// Default branch fallback if available
				auto defIt = switchBranches.find("Default");
				if (defIt != switchBranches.end() && defIt->second)
				{
					return defIt->second->EvaluateTracks(switchParams, blendParams);
				}
			}
			break;

		case ContainerType::Blend:
			if (!blendPoints.empty())
			{
				if (blendPoints.size() == 1)
				{
					if (blendPoints[0].second)
					{
						return blendPoints[0].second->EvaluateTracks(switchParams, blendParams);
					}
				}
				else
				{
					float paramVal = 0.0f;
					auto it = blendParams.find(blendParamName);
					if (it != blendParams.end()) paramVal = it->second;

					// Clamp to outer range
					if (paramVal <= blendPoints.front().first)
					{
						if (blendPoints.front().second)
							return blendPoints.front().second->EvaluateTracks(switchParams, blendParams);
					}
					else if (paramVal >= blendPoints.back().first)
					{
						if (blendPoints.back().second)
							return blendPoints.back().second->EvaluateTracks(switchParams, blendParams);
					}
					else
					{
						// Find enclosing span [i, i+1]
						for (size_t i = 0; i + 1 < blendPoints.size(); ++i)
						{
							float p0 = blendPoints[i].first;
							float p1 = blendPoints[i + 1].first;
							if (paramVal >= p0 && paramVal <= p1)
							{
								float span = std::max(1e-4f, p1 - p0);
								float t = std::clamp((paramVal - p0) / span, 0.0f, 1.0f);

								// Equal-power crossfade (cos/sin preserve RMS power)
								float w0 = std::cos(t * 1.57079632679f);
								float w1 = std::sin(t * 1.57079632679f);

								if (blendPoints[i].second)
								{
									auto t0 = blendPoints[i].second->EvaluateTracks(switchParams, blendParams);
									for (auto& trk : t0) trk.volumeScale *= w0;
									results.insert(results.end(), t0.begin(), t0.end());
								}
								if (blendPoints[i + 1].second)
								{
									auto t1 = blendPoints[i + 1].second->EvaluateTracks(switchParams, blendParams);
									for (auto& trk : t1) trk.volumeScale *= w1;
									results.insert(results.end(), t1.begin(), t1.end());
								}
								break;
							}
						}
					}
				}
			}
			break;

		case ContainerType::Layer:
			for (const auto& child : layerChildren)
			{
				if (child.second)
				{
					float layerVol = std::max(0.0f, child.first);
					auto sub = child.second->EvaluateTracks(switchParams, blendParams);
					for (auto& trk : sub)
					{
						trk.volumeScale *= layerVol;
					}
					results.insert(results.end(), sub.begin(), sub.end());
				}
			}
			break;
		}

		if (results.empty() && !variations.empty())
		{
			results.push_back({ variations[0].soundAssetPath, 1.0f, 0.0f });
		}
		return results;
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

	std::vector<EvaluatedSoundTrack> SoundEvent::EvaluateTracks(
		const std::unordered_map<std::string, std::string>& switchParams,
		const std::unordered_map<std::string, float>& blendParams) const
	{
		if (m_RootContainer)
		{
			return m_RootContainer->EvaluateTracks(switchParams, blendParams);
		}
		return {};
	}

	std::string SoundEvent::SerializeYaml() const
	{
		std::ostringstream ss;
		ss << "Id: \"" << m_Id << "\"\n";
		ss << "BusId: " << m_BusId << "\n";
		ss << "MaxInstances: " << m_MaxInstances << "\n";
		ss << "StealPolicy: " << static_cast<int>(m_StealPolicy) << "\n";
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

			if (m_RootContainer->type == ContainerType::Switch)
			{
				ss << "  SwitchParamName: \"" << m_RootContainer->switchParamName << "\"\n";
				ss << "  SwitchBranches:\n";
				for (const auto& [branch, node] : m_RootContainer->switchBranches)
				{
					if (node && !node->variations.empty())
					{
						ss << "    " << branch << ": \"" << node->variations[0].soundAssetPath << "\"\n";
					}
				}
			}
			else if (m_RootContainer->type == ContainerType::Blend)
			{
				ss << "  BlendParamName: \"" << m_RootContainer->blendParamName << "\"\n";
				ss << "  BlendPoints:\n";
				for (const auto& pt : m_RootContainer->blendPoints)
				{
					std::string p = (pt.second && !pt.second->variations.empty()) ? pt.second->variations[0].soundAssetPath : "";
					ss << "    - Threshold: " << pt.first << "\n";
					ss << "      Path: \"" << p << "\"\n";
				}
			}
			else if (m_RootContainer->type == ContainerType::Layer)
			{
				ss << "  LayerTracks:\n";
				for (const auto& layer : m_RootContainer->layerChildren)
				{
					std::string p = (layer.second && !layer.second->variations.empty()) ? layer.second->variations[0].soundAssetPath : "";
					ss << "    - Volume: " << layer.first << "\n";
					ss << "      Path: \"" << p << "\"\n";
				}
			}
			else
			{
				ss << "  Variations:\n";
				for (const auto& v : m_RootContainer->variations)
				{
					ss << "    - Path: \"" << v.soundAssetPath << "\"\n";
					ss << "      Weight: " << v.weight << "\n";
				}
			}
		}
		return ss.str();
	}

	bool SoundEvent::DeserializeYaml(const std::string& yamlText)
	{
		bool parsedAnyField = false;
		if (!m_RootContainer)
		{
			m_RootContainer = std::make_shared<SoundContainerNode>();
			m_RootContainer->type = ContainerType::Random;
		}
		else
		{
			m_RootContainer->variations.clear();
			m_RootContainer->switchBranches.clear();
			m_RootContainer->blendPoints.clear();
			m_RootContainer->layerChildren.clear();
		}

		std::istringstream stream(yamlText);
		std::string line;
		enum class YamlSection { Root, Spatial, RootContainer, Variations, SwitchBranches, BlendPoints, LayerTracks };
		YamlSection section = YamlSection::Root;

		float curThreshold = 0.0f;
		float curLayerVol = 1.0f;

		while (std::getline(stream, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();

			auto colonPos = line.find(':');
			if (colonPos == std::string::npos) continue;

			std::string key = line.substr(0, colonPos);
			while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
			if (!key.empty() && key.front() == '-')
			{
				key.erase(key.begin());
				while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
			}

			if (key == "Spatial") { section = YamlSection::Spatial; continue; }
			if (key == "RootContainer") { section = YamlSection::RootContainer; continue; }
			if (key == "Variations") { section = YamlSection::Variations; continue; }
			if (key == "SwitchBranches") { section = YamlSection::SwitchBranches; continue; }
			if (key == "BlendPoints") { section = YamlSection::BlendPoints; continue; }
			if (key == "LayerTracks") { section = YamlSection::LayerTracks; continue; }

			parsedAnyField = true;
			if (key == "Id")
			{
				auto q1 = line.find('"', colonPos);
				auto q2 = line.rfind('"');
				if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1)
				{
					m_Id = line.substr(q1 + 1, q2 - q1 - 1);
				}
				else
				{
					std::string v = line.substr(colonPos + 1);
					auto s = v.find_first_not_of(" \t");
					auto e = v.find_last_not_of(" \t\r\n");
					if (s != std::string::npos) m_Id = v.substr(s, e - s + 1);
				}
			}
			else if (key == "BusId") m_BusId = static_cast<BusId>(std::stoul(line.substr(colonPos + 1)));
			else if (key == "MaxInstances") m_MaxInstances = static_cast<uint32_t>(std::stoul(line.substr(colonPos + 1)));
			else if (key == "StealPolicy") m_StealPolicy = static_cast<StealPolicy>(std::stoul(line.substr(colonPos + 1)));
			else if (key == "CooldownSeconds") m_CooldownSeconds = std::stof(line.substr(colonPos + 1));
			else if (key == "VolumeRange")
			{
				auto lb = line.find('[', colonPos);
				auto cm = line.find(',', lb);
				auto rb = line.find(']', cm);
				if (lb != std::string::npos && cm != std::string::npos && rb != std::string::npos)
				{
					m_MinVolume = std::stof(line.substr(lb + 1, cm - lb - 1));
					m_MaxVolume = std::stof(line.substr(cm + 1, rb - cm - 1));
				}
			}
			else if (key == "PitchRange")
			{
				auto lb = line.find('[', colonPos);
				auto cm = line.find(',', lb);
				auto rb = line.find(']', cm);
				if (lb != std::string::npos && cm != std::string::npos && rb != std::string::npos)
				{
					m_MinPitchSemitones = std::stof(line.substr(lb + 1, cm - lb - 1));
					m_MaxPitchSemitones = std::stof(line.substr(cm + 1, rb - cm - 1));
				}
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
			else if (key == "AttenuationModel") m_Spatial.model = static_cast<AttenuationModel>(std::stoul(line.substr(colonPos + 1)));
			else if (key == "HalfViewWidth") m_Spatial.halfViewWidth = std::stof(line.substr(colonPos + 1));
			else if (key == "MaxPan") m_Spatial.maxPan = std::stof(line.substr(colonPos + 1));
			else if (key == "MinDistance") m_Spatial.minDistance = std::stof(line.substr(colonPos + 1));
			else if (key == "MaxDistance") m_Spatial.maxDistance = std::stof(line.substr(colonPos + 1));
			else if (key == "Type")
			{
				if (m_RootContainer) m_RootContainer->type = static_cast<ContainerType>(std::stoul(line.substr(colonPos + 1)));
			}
			else if (key == "ShuffleNoRepeat")
			{
				if (m_RootContainer) m_RootContainer->shuffleNoRepeat = (line.find("true") != std::string::npos);
			}
			else if (key == "SwitchParamName")
			{
				auto q1 = line.find('"', colonPos);
				auto q2 = line.rfind('"');
				if (q1 != std::string::npos && q2 > q1)
					m_RootContainer->switchParamName = line.substr(q1 + 1, q2 - q1 - 1);
				else
				{
					std::string v = line.substr(colonPos + 1);
					auto s = v.find_first_not_of(" \t");
					auto e = v.find_last_not_of(" \t\r\n");
					if (s != std::string::npos) m_RootContainer->switchParamName = v.substr(s, e - s + 1);
				}
			}
			else if (key == "BlendParamName")
			{
				auto q1 = line.find('"', colonPos);
				auto q2 = line.rfind('"');
				if (q1 != std::string::npos && q2 > q1)
					m_RootContainer->blendParamName = line.substr(q1 + 1, q2 - q1 - 1);
			}
			else if (key == "Threshold")
			{
				curThreshold = std::stof(line.substr(colonPos + 1));
			}
			else if (key == "Volume")
			{
				curLayerVol = std::stof(line.substr(colonPos + 1));
			}
			else if (key == "Path")
			{
				auto q1 = line.find('"', colonPos);
				auto q2 = line.rfind('"');
				if (q1 != std::string::npos && q2 > q1)
				{
					std::string p = line.substr(q1 + 1, q2 - q1 - 1);
					if (section == YamlSection::BlendPoints)
					{
						auto sub = std::make_shared<SoundContainerNode>();
						sub->type = ContainerType::Single;
						sub->variations.push_back({ p, 1.0f });
						m_RootContainer->blendPoints.push_back({ curThreshold, sub });
					}
					else if (section == YamlSection::LayerTracks)
					{
						auto sub = std::make_shared<SoundContainerNode>();
						sub->type = ContainerType::Single;
						sub->variations.push_back({ p, 1.0f });
						m_RootContainer->layerChildren.push_back({ curLayerVol, sub });
					}
					else
					{
						m_RootContainer->variations.push_back({ p, 1.0f });
					}
				}
			}
			else if (key == "Weight")
			{
				if (m_RootContainer && !m_RootContainer->variations.empty())
				{
					try { m_RootContainer->variations.back().weight = std::stof(line.substr(colonPos + 1)); }
					catch (...) { m_RootContainer->variations.back().weight = 1.0f; }
				}
			}
			else if (section == YamlSection::SwitchBranches)
			{
				auto q1 = line.find('"', colonPos);
				auto q2 = line.rfind('"');
				if (q1 != std::string::npos && q2 > q1)
				{
					std::string branchSound = line.substr(q1 + 1, q2 - q1 - 1);
					auto sub = std::make_shared<SoundContainerNode>();
					sub->type = ContainerType::Single;
					sub->variations.push_back({ branchSound, 1.0f });
					m_RootContainer->switchBranches[key] = sub;
				}
			}
		}
		const bool hasContent = !m_Id.empty() || (m_RootContainer && (
			!m_RootContainer->variations.empty() ||
			!m_RootContainer->switchBranches.empty() ||
			!m_RootContainer->blendPoints.empty() ||
			!m_RootContainer->layerChildren.empty()));
		return parsedAnyField && hasContent;
	}
}
