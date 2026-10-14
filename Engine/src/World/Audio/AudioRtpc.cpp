#include "AudioRtpc.h"

namespace World::Audio
{
	RtpcCurve::RtpcCurve(std::vector<RtpcCurvePoint> points)
		: m_Points(std::move(points))
	{
		std::sort(m_Points.begin(), m_Points.end(), [](const RtpcCurvePoint& a, const RtpcCurvePoint& b) {
			return a.inVal < b.inVal;
		});
	}

	void RtpcCurve::SetPoints(std::vector<RtpcCurvePoint> points)
	{
		m_Points = std::move(points);
		std::sort(m_Points.begin(), m_Points.end(), [](const RtpcCurvePoint& a, const RtpcCurvePoint& b) {
			return a.inVal < b.inVal;
		});
	}

	float RtpcCurve::Evaluate(float input) const noexcept
	{
		if (m_Points.empty()) return 1.0f;
		if (m_Points.size() == 1 || input <= m_Points.front().inVal)
			return m_Points.front().outVal;
		if (input >= m_Points.back().inVal)
			return m_Points.back().outVal;

		for (size_t i = 1; i < m_Points.size(); ++i)
		{
			if (input <= m_Points[i].inVal)
			{
				float x0 = m_Points[i - 1].inVal;
				float x1 = m_Points[i].inVal;
				float y0 = m_Points[i - 1].outVal;
				float y1 = m_Points[i].outVal;
				float t = (x1 > x0) ? ((input - x0) / (x1 - x0)) : 0.0f;
				return y0 + t * (y1 - y0);
			}
		}
		return m_Points.back().outVal;
	}

	void AudioRtpcManager::SetGlobalParameter(const std::string& name, float value)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_GlobalParameters[name] = value;
	}

	float AudioRtpcManager::GetGlobalParameter(const std::string& name) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_GlobalParameters.find(name);
		return (it != m_GlobalParameters.end()) ? it->second : 0.0f;
	}

	bool AudioRtpcManager::HasGlobalParameter(const std::string& name) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_GlobalParameters.find(name) != m_GlobalParameters.end();
	}

	void AudioRtpcManager::SetVoiceParameter(VoiceHandle handle, const std::string& name, float value)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_VoiceParameters[handle][name] = value;
	}

	float AudioRtpcManager::GetVoiceParameter(VoiceHandle handle, const std::string& name) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_VoiceParameters.find(handle);
		if (it != m_VoiceParameters.end())
		{
			auto pit = it->second.find(name);
			if (pit != it->second.end()) return pit->second;
		}
		auto git = m_GlobalParameters.find(name);
		return (git != m_GlobalParameters.end()) ? git->second : 0.0f;
	}

	void AudioRtpcManager::RemoveVoiceParameters(VoiceHandle handle)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_VoiceParameters.erase(handle);
	}

	void AudioRtpcManager::RegisterBinding(const std::string& paramName, RtpcTargetProperty target, const std::vector<RtpcCurvePoint>& points)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		RtpcBinding binding;
		binding.parameterName = paramName;
		binding.targetProperty = target;
		binding.curve.SetPoints(points);
		m_Bindings.push_back(std::move(binding));
	}

	void AudioRtpcManager::ClearBindings()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Bindings.clear();
	}

	void AudioRtpcManager::ApplyRtpc(VoiceHandle handle, float& inOutVolume, float& inOutPitch, float& inOutCutoff) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Bindings.empty()) return;

		for (const auto& binding : m_Bindings)
		{
			float paramVal = 0.0f;
			auto vIt = m_VoiceParameters.find(handle);
			if (vIt != m_VoiceParameters.end())
			{
				auto pIt = vIt->second.find(binding.parameterName);
				if (pIt != vIt->second.end()) paramVal = pIt->second;
				else
				{
					auto gIt = m_GlobalParameters.find(binding.parameterName);
					if (gIt != m_GlobalParameters.end()) paramVal = gIt->second;
				}
			}
			else
			{
				auto gIt = m_GlobalParameters.find(binding.parameterName);
				if (gIt != m_GlobalParameters.end()) paramVal = gIt->second;
			}

			float modVal = binding.curve.Evaluate(paramVal);
			switch (binding.targetProperty)
			{
			case RtpcTargetProperty::Volume:
				inOutVolume *= modVal;
				break;
			case RtpcTargetProperty::Pitch:
				inOutPitch *= modVal;
				break;
			case RtpcTargetProperty::LowPassCutoff:
				inOutCutoff = std::min(inOutCutoff, modVal);
				break;
			default:
				break;
			}
		}
	}
}
