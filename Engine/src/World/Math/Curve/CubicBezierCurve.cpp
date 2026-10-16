#include "wldpch.h"
#include "World/Math/Curve/CubicBezierCurve.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace World
{
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
		if (time <= m_Keyframes.front().Time) return m_Keyframes.front().Value;
		if (time >= m_Keyframes.back().Time) return m_Keyframes.back().Value;

		auto it = std::upper_bound(m_Keyframes.begin(), m_Keyframes.end(), time,
			[](float t, const Keyframe& k) { return t < k.Time; });

		size_t idx1 = static_cast<size_t>(std::distance(m_Keyframes.begin(), it));
		size_t idx0 = idx1 - 1;

		const auto& k0 = m_Keyframes[idx0];
		const auto& k1 = m_Keyframes[idx1];

		float dt = k1.Time - k0.Time;
		if (dt <= 1e-6f) return k0.Value;

		float u = (time - k0.Time) / dt;
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
