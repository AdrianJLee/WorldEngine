#pragma once

#include "World/Core/Export.h"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace World
{
	enum class TangentMode : uint8_t
	{
		Free = 0,
		Aligned = 1,
		Clamped = 2,
		Broken = 3
	};

	struct Keyframe
	{
		float Time = 0.0f;
		float Value = 0.0f;
		float InTangent = 0.0f;
		float OutTangent = 0.0f;
		TangentMode Mode = TangentMode::Aligned;

		Keyframe() = default;
		Keyframe(float time, float value, float inTangent = 0.0f, float outTangent = 0.0f, TangentMode mode = TangentMode::Aligned)
			: Time(time), Value(value), InTangent(inTangent), OutTangent(outTangent), Mode(mode) {}
	};

	class WLD_API CubicBezierCurve
	{
	public:
		CubicBezierCurve() = default;
		~CubicBezierCurve() = default;

		size_t AddKeyframe(const Keyframe& keyframe);
		bool RemoveKeyframe(size_t index);
		void Clear();

		size_t KeyframeCount() const { return m_Keyframes.size(); }
		const Keyframe& GetKeyframe(size_t index) const;
		Keyframe& GetKeyframeMut(size_t index);
		const std::vector<Keyframe>& GetKeyframes() const { return m_Keyframes; }

		float Evaluate(float time) const;

		void PrecomputeLut(size_t sampleCount = 256);
		float EvaluateFastLut(float normalizedT) const;
		const std::vector<float>& GetLut() const { return m_Lut; }

		static CubicBezierCurve Linear();
		static CubicBezierCurve EaseIn();
		static CubicBezierCurve EaseOut();
		static CubicBezierCurve EaseInOut();
		static CubicBezierCurve Bounce();

	private:
		std::vector<Keyframe> m_Keyframes;
		std::vector<float> m_Lut;
	};
}
