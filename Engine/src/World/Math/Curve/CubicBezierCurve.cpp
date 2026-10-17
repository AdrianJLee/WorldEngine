#include "wldpch.h"
#include "World/Math/Curve/CubicBezierCurve.h"
#include "World/Core/Log.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace World
{
	namespace
	{
		const char* ExtrapolationToString(CurveExtrapolation ext)
		{
			switch (ext)
			{
			case CurveExtrapolation::Clamp: return "Clamp";
			case CurveExtrapolation::Cycle: return "Cycle";
			case CurveExtrapolation::PingPong: return "PingPong";
			case CurveExtrapolation::Linear: return "Linear";
			default: return "Clamp";
			}
		}

		CurveExtrapolation StringToExtrapolation(const std::string& str)
		{
			if (str == "Cycle" || str == "1") return CurveExtrapolation::Cycle;
			if (str == "PingPong" || str == "2") return CurveExtrapolation::PingPong;
			if (str == "Linear" || str == "3") return CurveExtrapolation::Linear;
			return CurveExtrapolation::Clamp;
		}

		const char* TangentModeToString(TangentMode mode)
		{
			switch (mode)
			{
			case TangentMode::Free: return "Free";
			case TangentMode::Aligned: return "Aligned";
			case TangentMode::Clamped: return "Clamped";
			case TangentMode::Broken: return "Broken";
			default: return "Aligned";
			}
		}

		TangentMode StringToTangentMode(const std::string& str)
		{
			if (str == "Free" || str == "0") return TangentMode::Free;
			if (str == "Clamped" || str == "2") return TangentMode::Clamped;
			if (str == "Broken" || str == "3") return TangentMode::Broken;
			return TangentMode::Aligned;
		}

		std::filesystem::path ResolvePhysicalPath(const std::string& filepath)
		{
			if (filepath.empty())
				return {};
			std::error_code ec;
			std::filesystem::path p(filepath);
			if (std::filesystem::exists(p, ec))
				return p;
			std::filesystem::path fromProj = Paths::ProjectDir() / p;
			if (std::filesystem::exists(fromProj, ec))
				return fromProj;
			std::filesystem::path fromAsset = Paths::AssetRoot() / p;
			if (std::filesystem::exists(fromAsset, ec))
				return fromAsset;
			if (filepath.rfind("assets/", 0) == 0 || filepath.rfind("assets\\", 0) == 0)
			{
				std::filesystem::path stripped = Paths::AssetRoot() / filepath.substr(7);
				if (std::filesystem::exists(stripped, ec))
					return stripped;
			}
			return p;
		}
	}

	size_t CubicBezierCurve::AddKeyframe(const Keyframe& keyframe)
	{
		auto it = std::upper_bound(m_Keyframes.begin(), m_Keyframes.end(), keyframe.Time,
			[](float t, const Keyframe& k) { return t < k.Time; });
		size_t index = static_cast<size_t>(std::distance(m_Keyframes.begin(), it));
		m_Keyframes.insert(it, keyframe);
		return index;
	}

	bool CubicBezierCurve::RemoveKeyframe(size_t index)
	{
		if (index >= m_Keyframes.size())
			return false;
		m_Keyframes.erase(m_Keyframes.begin() + index);
		return true;
	}

	void CubicBezierCurve::Clear()
	{
		m_Keyframes.clear();
		m_Lut.clear();
	}

	const Keyframe& CubicBezierCurve::GetKeyframe(size_t index) const
	{
		if (index >= m_Keyframes.size())
			throw std::out_of_range("Keyframe index out of range");
		return m_Keyframes[index];
	}

	Keyframe& CubicBezierCurve::GetKeyframeMut(size_t index)
	{
		if (index >= m_Keyframes.size())
			throw std::out_of_range("Keyframe index out of range");
		return m_Keyframes[index];
	}

	float CubicBezierCurve::Evaluate(float time) const
	{
		if (m_Keyframes.empty()) return 0.0f;
		if (m_Keyframes.size() == 1) return m_Keyframes[0].Value;

		const float tStart = m_Keyframes.front().Time;
		const float tEnd = m_Keyframes.back().Time;
		const float span = tEnd - tStart;

		if (span <= 1e-6f) return m_Keyframes.front().Value;

		float evalTime = time;

		if (evalTime < tStart)
		{
			switch (PreExtrapolation)
			{
			case CurveExtrapolation::Clamp:
				return m_Keyframes.front().Value;
			case CurveExtrapolation::Linear:
			{
				const float dt = evalTime - tStart;
				return m_Keyframes.front().Value + dt * m_Keyframes.front().InTangent;
			}
			case CurveExtrapolation::Cycle:
			{
				const float offset = evalTime - tStart;
				float wrapped = std::fmod(offset, span);
				if (wrapped < 0.0f)
					wrapped += span;
				evalTime = tStart + wrapped;
				break;
			}
			case CurveExtrapolation::PingPong:
			{
				const float offset = tStart - evalTime;
				const int k = static_cast<int>(std::floor(offset / span));
				const float rem = offset - static_cast<float>(k) * span;
				if (k % 2 == 0)
					evalTime = tStart + rem;
				else
					evalTime = tEnd - rem;
				break;
			}
			}
		}
		else if (evalTime > tEnd)
		{
			switch (PostExtrapolation)
			{
			case CurveExtrapolation::Clamp:
				return m_Keyframes.back().Value;
			case CurveExtrapolation::Linear:
			{
				const float dt = evalTime - tEnd;
				return m_Keyframes.back().Value + dt * m_Keyframes.back().OutTangent;
			}
			case CurveExtrapolation::Cycle:
			{
				const float offset = evalTime - tStart;
				float wrapped = std::fmod(offset, span);
				if (wrapped < 0.0f)
					wrapped += span;
				evalTime = tStart + wrapped;
				break;
			}
			case CurveExtrapolation::PingPong:
			{
				const float offset = evalTime - tEnd;
				const int k = static_cast<int>(std::floor(offset / span));
				const float rem = offset - static_cast<float>(k) * span;
				if (k % 2 == 0)
					evalTime = tEnd - rem;
				else
					evalTime = tStart + rem;
				break;
			}
			}
		}

		evalTime = std::clamp(evalTime, tStart, tEnd);

		if (evalTime <= tStart) return m_Keyframes.front().Value;
		if (evalTime >= tEnd) return m_Keyframes.back().Value;

		auto it = std::upper_bound(m_Keyframes.begin(), m_Keyframes.end(), evalTime,
			[](float t, const Keyframe& k) { return t < k.Time; });

		size_t idx1 = static_cast<size_t>(std::distance(m_Keyframes.begin(), it));
		size_t idx0 = idx1 - 1;

		const auto& k0 = m_Keyframes[idx0];
		const auto& k1 = m_Keyframes[idx1];

		float dt = k1.Time - k0.Time;
		if (dt <= 1e-6f) return k0.Value;

		float u = (evalTime - k0.Time) / dt;
		u = std::clamp(u, 0.0f, 1.0f);

		float u2 = u * u;
		float u3 = u2 * u;

		float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
		float h10 = u3 - 2.0f * u2 + u;
		float h01 = -2.0f * u3 + 3.0f * u2;
		float h11 = u3 - u2;

		float m0 = k0.OutTangent * dt;
		float m1 = k1.InTangent * dt;

		return h00 * k0.Value + h10 * m0 + h01 * k1.Value + h11 * m1;
	}

	void CubicBezierCurve::AutoSmoothTangents()
	{
		const size_t count = m_Keyframes.size();
		if (count <= 1)
			return;

		if (count == 2)
		{
			const float dt = m_Keyframes[1].Time - m_Keyframes[0].Time;
			const float slope = (dt > 1e-6f) ? ((m_Keyframes[1].Value - m_Keyframes[0].Value) / dt) : 0.0f;
			m_Keyframes[0].InTangent = slope;
			m_Keyframes[0].OutTangent = slope;
			m_Keyframes[0].Mode = TangentMode::Aligned;
			m_Keyframes[1].InTangent = slope;
			m_Keyframes[1].OutTangent = slope;
			m_Keyframes[1].Mode = TangentMode::Aligned;
		}
		else
		{
			// First keyframe: slope to next
			{
				const float dt = m_Keyframes[1].Time - m_Keyframes[0].Time;
				const float slope = (dt > 1e-6f) ? ((m_Keyframes[1].Value - m_Keyframes[0].Value) / dt) : 0.0f;
				m_Keyframes[0].InTangent = slope;
				m_Keyframes[0].OutTangent = slope;
				m_Keyframes[0].Mode = TangentMode::Aligned;
			}

			// Middle keyframes: Catmull-Rom central difference
			for (size_t i = 1; i < count - 1; ++i)
			{
				const float dt = m_Keyframes[i + 1].Time - m_Keyframes[i - 1].Time;
				const float slope = (dt > 1e-6f) ? ((m_Keyframes[i + 1].Value - m_Keyframes[i - 1].Value) / dt) : 0.0f;
				m_Keyframes[i].InTangent = slope;
				m_Keyframes[i].OutTangent = slope;
				m_Keyframes[i].Mode = TangentMode::Aligned;
			}

			// Last keyframe: slope from previous
			{
				const float dt = m_Keyframes[count - 1].Time - m_Keyframes[count - 2].Time;
				const float slope = (dt > 1e-6f) ? ((m_Keyframes[count - 1].Value - m_Keyframes[count - 2].Value) / dt) : 0.0f;
				m_Keyframes[count - 1].InTangent = slope;
				m_Keyframes[count - 1].OutTangent = slope;
				m_Keyframes[count - 1].Mode = TangentMode::Aligned;
			}
		}

		if (!m_Lut.empty())
		{
			PrecomputeLut(m_Lut.size());
		}
	}

	int CubicBezierCurve::FindNearestKeyframe(float time, float value, float timeTol, float valTol) const
	{
		if (m_Keyframes.empty())
			return -1;

		int bestIndex = -1;
		float bestDistSq = std::numeric_limits<float>::max();

		const float safeTimeTol = (timeTol > 1e-6f) ? timeTol : 1e-6f;
		const float safeValTol = (valTol > 1e-6f) ? valTol : 1e-6f;

		for (size_t i = 0; i < m_Keyframes.size(); ++i)
		{
			const auto& k = m_Keyframes[i];
			const float dt = std::abs(k.Time - time);
			const float dv = std::abs(k.Value - value);

			if (dt <= timeTol && dv <= valTol)
			{
				const float ndt = dt / safeTimeTol;
				const float ndv = dv / safeValTol;
				const float distSq = ndt * ndt + ndv * ndv;
				if (distSq < bestDistSq)
				{
					bestDistSq = distSq;
					bestIndex = static_cast<int>(i);
				}
			}
		}

		return bestIndex;
	}

	std::string CubicBezierCurve::SerializeToYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "FormatVersion" << YAML::Value << 1;
		out << YAML::Key << "PreExtrapolation" << YAML::Value << ExtrapolationToString(PreExtrapolation);
		out << YAML::Key << "PostExtrapolation" << YAML::Value << ExtrapolationToString(PostExtrapolation);
		out << YAML::Key << "Keyframes" << YAML::Value << YAML::BeginSeq;
		for (const auto& k : m_Keyframes)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Time" << YAML::Value << k.Time;
			out << YAML::Key << "Value" << YAML::Value << k.Value;
			out << YAML::Key << "InTangent" << YAML::Value << k.InTangent;
			out << YAML::Key << "OutTangent" << YAML::Value << k.OutTangent;
			out << YAML::Key << "Mode" << YAML::Value << TangentModeToString(k.Mode);
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::EndMap;
		return std::string(out.c_str());
	}

	bool CubicBezierCurve::DeserializeFromYaml(const std::string& yaml)
	{
		try
		{
			YAML::Node root = YAML::Load(yaml);
			if (!root || !root.IsMap())
				return false;

			if (root["PreExtrapolation"] && root["PreExtrapolation"].IsScalar())
				PreExtrapolation = StringToExtrapolation(root["PreExtrapolation"].as<std::string>());
			if (root["PostExtrapolation"] && root["PostExtrapolation"].IsScalar())
				PostExtrapolation = StringToExtrapolation(root["PostExtrapolation"].as<std::string>());

			m_Keyframes.clear();
			YAML::Node keyframesNode = root["Keyframes"];
			if (keyframesNode && keyframesNode.IsSequence())
			{
				for (const auto& kNode : keyframesNode)
				{
					Keyframe k;
					if (kNode["Time"]) k.Time = kNode["Time"].as<float>();
					if (kNode["Value"]) k.Value = kNode["Value"].as<float>();
					if (kNode["InTangent"]) k.InTangent = kNode["InTangent"].as<float>();
					if (kNode["OutTangent"]) k.OutTangent = kNode["OutTangent"].as<float>();
					if (kNode["Mode"] && kNode["Mode"].IsScalar())
						k.Mode = StringToTangentMode(kNode["Mode"].as<std::string>());
					m_Keyframes.push_back(k);
				}
			}

			std::sort(m_Keyframes.begin(), m_Keyframes.end(),
				[](const Keyframe& a, const Keyframe& b) { return a.Time < b.Time; });

			if (!m_Lut.empty())
				PrecomputeLut(m_Lut.size());

			return true;
		}
		catch (const std::exception& e)
		{
			WLD_CORE_WARN("[CubicBezierCurve] Failed to deserialize YAML: {}", e.what());
			return false;
		}
	}

	bool CubicBezierCurve::SaveToFile(const std::string& filepath) const
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
			WLD_CORE_WARN("[CubicBezierCurve] Failed to write file: {}", filepath);
			return false;
		}
		file << SerializeToYaml();
		return true;
	}

	bool CubicBezierCurve::LoadFromFile(const std::string& filepath)
	{
		const auto resolved = ResolvePhysicalPath(filepath);
		std::ifstream file(resolved.string());
		if (!file.is_open())
		{
			WLD_CORE_WARN("[CubicBezierCurve] Failed to open file: {}", filepath);
			return false;
		}
		std::stringstream ss;
		ss << file.rdbuf();
		return DeserializeFromYaml(ss.str());
	}

	void CubicBezierCurve::PrecomputeLut(size_t sampleCount)
	{
		if (sampleCount < 2) sampleCount = 2;
		m_Lut.resize(sampleCount);

		if (m_Keyframes.empty())
		{
			std::fill(m_Lut.begin(), m_Lut.end(), 0.0f);
			return;
		}

		float tStart = m_Keyframes.front().Time;
		float tEnd = m_Keyframes.back().Time;
		float span = tEnd - tStart;

		for (size_t i = 0; i < sampleCount; ++i)
		{
			float s = static_cast<float>(i) / static_cast<float>(sampleCount - 1);
			float t = tStart + s * span;
			m_Lut[i] = Evaluate(t);
		}
	}

	float CubicBezierCurve::EvaluateFastLut(float normalizedT) const
	{
		if (m_Lut.empty())
		{
			if (m_Keyframes.empty()) return 0.0f;
			float tStart = m_Keyframes.front().Time;
			float tEnd = m_Keyframes.back().Time;
			float t = tStart + std::clamp(normalizedT, 0.0f, 1.0f) * (tEnd - tStart);
			return Evaluate(t);
		}

		if (m_Lut.size() == 1) return m_Lut[0];

		normalizedT = std::clamp(normalizedT, 0.0f, 1.0f);
		float fIdx = normalizedT * static_cast<float>(m_Lut.size() - 1);
		size_t idx = static_cast<size_t>(fIdx);
		if (idx >= m_Lut.size() - 1) return m_Lut.back();

		float frac = fIdx - static_cast<float>(idx);
		return (1.0f - frac) * m_Lut[idx] + frac * m_Lut[idx + 1];
	}

	CubicBezierCurve CubicBezierCurve::Linear()
	{
		CubicBezierCurve curve;
		curve.AddKeyframe(Keyframe(0.0f, 0.0f, 1.0f, 1.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(1.0f, 1.0f, 1.0f, 1.0f, TangentMode::Aligned));
		curve.PrecomputeLut(256);
		return curve;
	}

	CubicBezierCurve CubicBezierCurve::EaseIn()
	{
		CubicBezierCurve curve;
		curve.AddKeyframe(Keyframe(0.0f, 0.0f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(1.0f, 1.0f, 2.0f, 2.0f, TangentMode::Aligned));
		curve.PrecomputeLut(256);
		return curve;
	}

	CubicBezierCurve CubicBezierCurve::EaseOut()
	{
		CubicBezierCurve curve;
		curve.AddKeyframe(Keyframe(0.0f, 0.0f, 2.0f, 2.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(1.0f, 1.0f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.PrecomputeLut(256);
		return curve;
	}

	CubicBezierCurve CubicBezierCurve::EaseInOut()
	{
		CubicBezierCurve curve;
		curve.AddKeyframe(Keyframe(0.0f, 0.0f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(1.0f, 1.0f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.PrecomputeLut(256);
		return curve;
	}

	CubicBezierCurve CubicBezierCurve::Bounce()
	{
		CubicBezierCurve curve;
		curve.AddKeyframe(Keyframe(0.00f, 0.00f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(0.35f, 1.00f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(0.55f, 0.75f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(0.75f, 1.00f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(0.90f, 0.95f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.AddKeyframe(Keyframe(1.00f, 1.00f, 0.0f, 0.0f, TangentMode::Aligned));
		curve.PrecomputeLut(256);
		return curve;
	}
}
