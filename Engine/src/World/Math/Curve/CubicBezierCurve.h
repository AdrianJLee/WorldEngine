#pragma once

#include "World/Core/Export.h"
#include <cstdint>
#include <cstddef>
#include <string>
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

	enum class CurveExtrapolation : uint8_t
	{
		Clamp = 0,    // 保持边界首末值
		Cycle = 1,    // 周期循环 (t 自动取模)
		PingPong = 2, // 往返反弹
		Linear = 3    // 按端点切线斜率线性向外延伸
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
		CurveExtrapolation PreExtrapolation = CurveExtrapolation::Clamp;
		CurveExtrapolation PostExtrapolation = CurveExtrapolation::Clamp;

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

		void AutoSmoothTangents();
		int FindNearestKeyframe(float time, float value, float timeTol = 0.05f, float valTol = 0.05f) const;

		bool SaveToFile(const std::string& filepath) const;
		bool LoadFromFile(const std::string& filepath);
		std::string SerializeToYaml() const;
		bool DeserializeFromYaml(const std::string& yaml);

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
