#include "wldpch.h"
#include "World/Math/Curve/ColorGradient.h"
#include "World/Utils/Paths.h"
#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace World
{
	namespace
	{
		std::filesystem::path ResolvePhysicalPath(const std::string& filepath)
		{
			if (filepath.empty())
				return {};
			std::error_code ec;
			std::filesystem::path p(filepath);
			if (p.is_absolute() && std::filesystem::exists(p, ec))
				return p;

			std::string stripped = filepath;
			if (stripped.rfind("assets/", 0) == 0 || stripped.rfind("assets\\", 0) == 0)
				stripped = stripped.substr(7);

			std::filesystem::path fromAsset = Paths::AssetRoot() / stripped;
			if (std::filesystem::exists(fromAsset, ec))
				return fromAsset;

			std::filesystem::path fromProj = Paths::ProjectDir() / stripped;
			if (std::filesystem::exists(fromProj, ec))
				return fromProj;

			return p;
		}
	}

	void ColorGradient::AddColorStop(float time, const glm::vec3& color)
	{
		m_ColorStops.push_back(ColorStop(time, color));
		std::sort(m_ColorStops.begin(), m_ColorStops.end(),
			[](const ColorStop& a, const ColorStop& b) { return a.Time < b.Time; });

		if (!m_Lut.empty())
		{
			PrecomputeLut(m_Lut.size());
		}
	}

	void ColorGradient::AddAlphaStop(float time, float alpha)
	{
		m_AlphaStops.push_back(AlphaStop(time, alpha));
		std::sort(m_AlphaStops.begin(), m_AlphaStops.end(),
			[](const AlphaStop& a, const AlphaStop& b) { return a.Time < b.Time; });

		if (!m_Lut.empty())
		{
			PrecomputeLut(m_Lut.size());
		}
	}

	void ColorGradient::Clear()
	{
		m_ColorStops.clear();
		m_AlphaStops.clear();
		m_Lut.clear();
	}

	glm::vec4 ColorGradient::Evaluate(float time) const
	{
		glm::vec3 rgb(1.0f);
		if (!m_ColorStops.empty())
		{
			if (m_ColorStops.size() == 1 || time <= m_ColorStops.front().Time)
			{
				rgb = m_ColorStops.front().Color;
			}
			else if (time >= m_ColorStops.back().Time)
			{
				rgb = m_ColorStops.back().Color;
			}
			else
			{
				for (size_t i = 0; i + 1 < m_ColorStops.size(); ++i)
				{
					if (time >= m_ColorStops[i].Time && time <= m_ColorStops[i + 1].Time)
					{
						float span = m_ColorStops[i + 1].Time - m_ColorStops[i].Time;
						float factor = (span > 1e-6f) ? (time - m_ColorStops[i].Time) / span : 0.0f;
						rgb = glm::mix(m_ColorStops[i].Color, m_ColorStops[i + 1].Color, factor);
						break;
					}
				}
			}
		}

		float a = 1.0f;
		if (!m_AlphaStops.empty())
		{
			if (m_AlphaStops.size() == 1 || time <= m_AlphaStops.front().Time)
			{
				a = m_AlphaStops.front().Alpha;
			}
			else if (time >= m_AlphaStops.back().Time)
			{
				a = m_AlphaStops.back().Alpha;
			}
			else
			{
				for (size_t i = 0; i + 1 < m_AlphaStops.size(); ++i)
				{
					if (time >= m_AlphaStops[i].Time && time <= m_AlphaStops[i + 1].Time)
					{
						float span = m_AlphaStops[i + 1].Time - m_AlphaStops[i].Time;
						float factor = (span > 1e-6f) ? (time - m_AlphaStops[i].Time) / span : 0.0f;
						a = glm::mix(m_AlphaStops[i].Alpha, m_AlphaStops[i + 1].Alpha, factor);
						break;
					}
				}
			}
		}

		return glm::vec4(rgb, a);
	}

	void ColorGradient::PrecomputeLut(size_t sampleCount)
	{
		if (sampleCount < 2) sampleCount = 2;
		m_Lut.resize(sampleCount);

		for (size_t i = 0; i < sampleCount; ++i)
		{
			float t = static_cast<float>(i) / static_cast<float>(sampleCount - 1);
			m_Lut[i] = Evaluate(t);
		}
	}

	glm::vec4 ColorGradient::EvaluateFastLut(float normalizedT) const
	{
		if (m_Lut.empty())
		{
			return Evaluate(normalizedT);
		}

		if (m_Lut.size() == 1)
		{
			return m_Lut[0];
		}

		normalizedT = glm::clamp(normalizedT, 0.0f, 1.0f);
		float fIdx = normalizedT * static_cast<float>(m_Lut.size() - 1);
		size_t idx = static_cast<size_t>(fIdx);
		if (idx >= m_Lut.size() - 1)
		{
			return m_Lut.back();
		}

		float frac = fIdx - static_cast<float>(idx);
		return glm::mix(m_Lut[idx], m_Lut[idx + 1], frac);
	}

	std::string ColorGradient::SerializeToYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "FormatVersion" << YAML::Value << 1;
		out << YAML::Key << "ColorStops" << YAML::Value << YAML::BeginSeq;
		for (const auto& c : m_ColorStops)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Time" << YAML::Value << c.Time;
			out << YAML::Key << "Color" << YAML::Value << YAML::Flow << YAML::BeginSeq
				<< c.Color.r << c.Color.g << c.Color.b << YAML::EndSeq;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;

		out << YAML::Key << "AlphaStops" << YAML::Value << YAML::BeginSeq;
		for (const auto& a : m_AlphaStops)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Time" << YAML::Value << a.Time;
			out << YAML::Key << "Alpha" << YAML::Value << a.Alpha;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::EndMap;

		return std::string(out.c_str());
	}

	bool ColorGradient::DeserializeFromYaml(const std::string& yaml)
	{
		try
		{
			YAML::Node root = YAML::Load(yaml);
			if (!root || !root.IsMap())
				return false;

			Clear();
			if (root["ColorStops"] && root["ColorStops"].IsSequence())
			{
				for (const auto& node : root["ColorStops"])
				{
					float t = node["Time"] ? node["Time"].as<float>() : 0.0f;
					glm::vec3 col(1.0f);
					if (node["Color"] && node["Color"].IsSequence() && node["Color"].size() >= 3)
					{
						col.r = node["Color"][0].as<float>();
						col.g = node["Color"][1].as<float>();
						col.b = node["Color"][2].as<float>();
					}
					m_ColorStops.push_back(ColorStop(t, col));
				}
			}

			if (root["AlphaStops"] && root["AlphaStops"].IsSequence())
			{
				for (const auto& node : root["AlphaStops"])
				{
					float t = node["Time"] ? node["Time"].as<float>() : 0.0f;
					float a = node["Alpha"] ? node["Alpha"].as<float>() : 1.0f;
					m_AlphaStops.push_back(AlphaStop(t, a));
				}
			}

			std::sort(m_ColorStops.begin(), m_ColorStops.end(),
				[](const ColorStop& a, const ColorStop& b) { return a.Time < b.Time; });
			std::sort(m_AlphaStops.begin(), m_AlphaStops.end(),
				[](const AlphaStop& a, const AlphaStop& b) { return a.Time < b.Time; });

			PrecomputeLut(256);
			return true;
		}
		catch (const std::exception& e)
		{
			WLD_CORE_WARN("[ColorGradient] Failed to deserialize YAML: {}", e.what());
			return false;
		}
	}

	bool ColorGradient::SaveToFile(const std::string& filepath) const
	{
		std::filesystem::path p(filepath);
		if (p.has_parent_path())
		{
			std::error_code ec;
			std::filesystem::create_directories(p.parent_path(), ec);
		}

		std::ofstream file(filepath);
		if (!file.is_open())
		{
			WLD_CORE_WARN("[ColorGradient] Failed to write file: {}", filepath);
			return false;
		}
		file << SerializeToYaml();
		return true;
	}

	bool ColorGradient::LoadFromFile(const std::string& filepath)
	{
		const auto resolved = ResolvePhysicalPath(filepath);
		std::ifstream file(resolved.string());
		if (!file.is_open())
		{
			WLD_CORE_WARN("[ColorGradient] Failed to open file: {}", filepath);
			return false;
		}
		std::stringstream ss;
		ss << file.rdbuf();
		return DeserializeFromYaml(ss.str());
	}

	ColorGradient ColorGradient::DefaultFire()
	{
		ColorGradient g;
		g.AddColorStop(0.0f, glm::vec3(1.0f, 1.0f, 0.8f));
		g.AddColorStop(0.2f, glm::vec3(1.0f, 0.6f, 0.1f));
		g.AddColorStop(0.6f, glm::vec3(0.8f, 0.15f, 0.02f));
		g.AddColorStop(1.0f, glm::vec3(0.1f, 0.1f, 0.1f));
		g.AddAlphaStop(0.0f, 1.0f);
		g.AddAlphaStop(0.3f, 0.9f);
		g.AddAlphaStop(0.7f, 0.5f);
		g.AddAlphaStop(1.0f, 0.0f);
		g.PrecomputeLut(256);
		return g;
	}

	ColorGradient ColorGradient::DefaultSmoke()
	{
		ColorGradient g;
		g.AddColorStop(0.0f, glm::vec3(0.75f, 0.75f, 0.75f));
		g.AddColorStop(0.3f, glm::vec3(0.55f, 0.55f, 0.55f));
		g.AddColorStop(0.7f, glm::vec3(0.3f, 0.3f, 0.3f));
		g.AddColorStop(1.0f, glm::vec3(0.15f, 0.15f, 0.15f));
		g.AddAlphaStop(0.0f, 0.0f);
		g.AddAlphaStop(0.2f, 0.7f);
		g.AddAlphaStop(0.8f, 0.4f);
		g.AddAlphaStop(1.0f, 0.0f);
		g.PrecomputeLut(256);
		return g;
	}
}
