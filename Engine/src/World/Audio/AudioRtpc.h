#pragma once

#include "AudioTypes.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>

namespace World::Audio
{
	enum class RtpcTargetProperty : uint8_t
	{
		Volume = 0,
		Pitch = 1,
		LowPassCutoff = 2,
		HighPassCutoff = 3,
		ReverbSend = 4,
		Pan = 5
	};

	struct RtpcCurvePoint
	{
		float inVal { 0.0f };
		float outVal { 0.0f };
	};

	class WLD_API RtpcCurve
	{
	public:
		RtpcCurve() = default;
		explicit RtpcCurve(std::vector<RtpcCurvePoint> points);

		void SetPoints(std::vector<RtpcCurvePoint> points);
		const std::vector<RtpcCurvePoint>& GetPoints() const noexcept { return m_Points; }
		float Evaluate(float input) const noexcept;

	private:
		std::vector<RtpcCurvePoint> m_Points;
	};

	struct RtpcBinding
	{
		std::string parameterName;
		RtpcTargetProperty targetProperty { RtpcTargetProperty::Volume };
		RtpcCurve curve;
	};

	class WLD_API AudioRtpcManager
	{
	public:
		AudioRtpcManager() = default;

		void SetGlobalParameter(const std::string& name, float value);
		float GetGlobalParameter(const std::string& name) const;
		bool HasGlobalParameter(const std::string& name) const;

		void SetVoiceParameter(VoiceHandle handle, const std::string& name, float value);
		float GetVoiceParameter(VoiceHandle handle, const std::string& name) const;
		void RemoveVoiceParameters(VoiceHandle handle);

		void RegisterBinding(const std::string& paramName, RtpcTargetProperty target, const std::vector<RtpcCurvePoint>& points);
		void ClearBindings();

		void ApplyRtpc(VoiceHandle handle, float& inOutVolume, float& inOutPitch, float& inOutCutoff) const;

	private:
		mutable std::mutex m_Mutex;
		std::unordered_map<std::string, float> m_GlobalParameters;
		std::unordered_map<VoiceHandle, std::unordered_map<std::string, float>> m_VoiceParameters;
		std::vector<RtpcBinding> m_Bindings;
	};
}
