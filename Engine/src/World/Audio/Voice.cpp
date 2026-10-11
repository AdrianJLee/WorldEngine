#include "Voice.h"

namespace World::Audio
{
	void Voice::Reset() noexcept
	{
		m_Handle = kInvalidVoiceHandle;
		m_BusId = 0;
		m_State = VoiceState::Free;
		m_Buffer.reset();
		m_CursorFrame = 0.0;
		m_BaseVolume = 1.0f;
		m_Pitch = 1.0f;
		m_Looping = false;
		m_Priority = 128;
		m_IsSpatial = false;
		m_LeftGain = 0.7071f;
		m_RightGain = 0.7071f;
		m_CurrentAudibility = 1.0f;
		m_Occlusion = 0.0f;
		m_FilterLeft.Reset();
		m_FilterRight.Reset();
		m_FadeRemainingFrames = 0;
		m_FadeTotalFrames = 0;
	}

	void Voice::Setup(
		VoiceHandle handle,
		const std::shared_ptr<AudioBuffer>& buffer,
		BusId busId,
		float volume,
		float pitch,
		bool loop,
		uint8_t priority) noexcept
	{
		Reset();
		m_Handle = handle;
		m_Buffer = buffer;
		m_BusId = busId;
		m_BaseVolume = std::clamp(volume, 0.0f, 4.0f);
		m_Pitch = std::clamp(pitch, 0.1f, 4.0f);
		m_Looping = loop;
		m_Priority = priority;
		m_State = (buffer && buffer->GetFrameCount() > 0) ? VoiceState::Playing : VoiceState::Free;
		m_CurrentAudibility = m_BaseVolume;
	}

	void Voice::Stop(float fadeOutSeconds) noexcept
	{
		if (m_State == VoiceState::Free) return;
		if (fadeOutSeconds <= 0.001f)
		{
			Reset();
			return;
		}

		m_State = VoiceState::Stopping;
		uint32_t fadeFrames = static_cast<uint32_t>(fadeOutSeconds * static_cast<float>(kDefaultSampleRate));
		m_FadeTotalFrames = std::max(1u, fadeFrames);
		m_FadeRemainingFrames = m_FadeTotalFrames;
	}

	void Voice::Pause() noexcept
	{
		if (m_State == VoiceState::Playing) m_State = VoiceState::Paused;
	}

	void Voice::Resume() noexcept
	{
		if (m_State == VoiceState::Paused) m_State = VoiceState::Playing;
	}

	void Voice::SetSpatialParameters(
		const glm::vec3& position,
		const glm::vec3& forward,
		float minDistance,
		float maxDistance,
		AttenuationModel model,
		float innerAngle,
		float outerAngle,
		float outerGain) noexcept
	{
		m_IsSpatial = true;
		m_Position = position;
		m_Forward = forward;
		m_MinDistance = std::max(0.1f, minDistance);
		m_MaxDistance = std::max(m_MinDistance + 0.1f, maxDistance);
		m_AttenuationModel = model;
		m_InnerAngle = innerAngle;
		m_OuterAngle = outerAngle;
		m_OuterGain = outerGain;
	}

	void Voice::SetOcclusion(float occlusion01) noexcept
	{
		m_Occlusion = std::clamp(occlusion01, 0.0f, 1.0f);
	}

	void Voice::SetPitch(float pitch) noexcept
	{
		m_Pitch = std::clamp(pitch, 0.1f, 4.0f);
	}

	void Voice::SetVolume(float volume) noexcept
	{
		m_BaseVolume = std::clamp(volume, 0.0f, 4.0f);
	}

	void Voice::UpdateSpatial(
		const glm::vec3& listenerPos,
		const glm::vec3& listenerRight,
		float sampleRate) noexcept
	{
		if (!m_IsSpatial)
		{
			m_LeftGain = m_BaseVolume * 0.7071f;
			m_RightGain = m_BaseVolume * 0.7071f;
			m_CurrentAudibility = m_BaseVolume;
			return;
		}

		float dist = glm::distance(m_Position, listenerPos);
		float distAtten = SpatialCalculator::CalculateDistanceAttenuation(
			dist, m_MinDistance, m_MaxDistance, m_AttenuationModel);

		float coneAtten = SpatialCalculator::CalculateConeAttenuation(
			m_Position, m_Forward, listenerPos, m_InnerAngle, m_OuterAngle, m_OuterGain);

		float panLeft = 0.7071f;
		float panRight = 0.7071f;
		SpatialCalculator::CalculateStereoPan(m_Position, listenerPos, listenerRight, panLeft, panRight);

		// Occlusion low-pass filter cutoff frequency: drops from 20000Hz to 600Hz as occlusion goes 0 -> 1
		float cutoffFreq = 20000.0f * (1.0f - m_Occlusion * 0.95f) + 400.0f;
		m_FilterLeft.Configure(FilterType::LowPass, cutoffFreq, sampleRate);
		m_FilterRight.Configure(FilterType::LowPass, cutoffFreq, sampleRate);

		float occlusionGain = 1.0f - m_Occlusion * 0.4f; // 40% volume reduction on full occlusion
		float totalGain = m_BaseVolume * distAtten * coneAtten * occlusionGain;

		m_LeftGain = panLeft * totalGain;
		m_RightGain = panRight * totalGain;
		m_CurrentAudibility = totalGain;

		// Voice virtualization trigger: if totalGain is inaudible (<-60dB), demote to Virtual
		if (m_CurrentAudibility < 0.001f && m_State == VoiceState::Playing)
		{
			m_State = VoiceState::Virtual;
		}
		else if (m_CurrentAudibility >= 0.001f && m_State == VoiceState::Virtual)
		{
			m_State = VoiceState::Playing;
		}
	}

	bool Voice::RenderBlock(
		float* outLeft,
		float* outRight,
		uint32_t frameCount,
		float sampleRate) noexcept
	{
		if (m_State != VoiceState::Playing && m_State != VoiceState::Stopping)
		{
			return m_State != VoiceState::Free;
		}
		if (!m_Buffer)
		{
			m_State = VoiceState::Free;
			return false;
		}

		uint32_t bufFrames = m_Buffer->GetFrameCount();
		uint32_t bufChannels = m_Buffer->GetChannels();
		if (bufFrames == 0)
		{
			m_State = VoiceState::Free;
			return false;
		}

		// Resampling ratio
		double bufferSampleRate = static_cast<double>(m_Buffer->GetSampleRate());
		double outputSampleRate = static_cast<double>(sampleRate);
		double step = (bufferSampleRate / outputSampleRate) * static_cast<double>(m_Pitch);

		uint32_t loopStart = m_Buffer->GetLoopStartFrame();
		uint32_t loopEnd = m_Buffer->HasLoopPoints() ? m_Buffer->GetLoopEndFrame() : bufFrames;

		for (uint32_t i = 0; i < frameCount; ++i)
		{
			uint32_t frame0 = static_cast<uint32_t>(m_CursorFrame);
			uint32_t frame1 = frame0 + 1;
			float frac = static_cast<float>(m_CursorFrame - frame0);

			if (frame0 >= bufFrames)
			{
				if (m_Looping)
				{
					m_CursorFrame = static_cast<double>(loopStart);
					frame0 = loopStart;
					frame1 = frame0 + 1;
				}
				else
				{
					m_State = VoiceState::Free;
					return false;
				}
			}
			if (frame1 >= bufFrames)
			{
				frame1 = m_Looping ? loopStart : (bufFrames - 1);
			}

			float inL = 0.0f;
			float inR = 0.0f;

			if (bufChannels == 1)
			{
				float s0 = m_Buffer->GetSample(0, frame0);
				float s1 = m_Buffer->GetSample(0, frame1);
				float s = s0 + frac * (s1 - s0);
				inL = s;
				inR = s;
			}
			else
			{
				float l0 = m_Buffer->GetSample(0, frame0);
				float l1 = m_Buffer->GetSample(0, frame1);
				float r0 = m_Buffer->GetSample(1, frame0);
				float r1 = m_Buffer->GetSample(1, frame1);
				inL = l0 + frac * (l1 - l0);
				inR = r0 + frac * (r1 - r0);
			}

			// Apply fade-out if stopping
			float fadeMultiplier = 1.0f;
			if (m_State == VoiceState::Stopping && m_FadeTotalFrames > 0)
			{
				fadeMultiplier = static_cast<float>(m_FadeRemainingFrames) / static_cast<float>(m_FadeTotalFrames);
				if (m_FadeRemainingFrames > 0) --m_FadeRemainingFrames;
				else
				{
					m_State = VoiceState::Free;
					return false;
				}
			}

			// Apply DSP filters & gains
			float filteredL = m_FilterLeft.Process(inL);
			float filteredR = m_FilterRight.Process(inR);

			outLeft[i] += filteredL * m_LeftGain * fadeMultiplier;
			outRight[i] += filteredR * m_RightGain * fadeMultiplier;

			m_CursorFrame += step;
		}

		return true;
	}

	bool Voice::AdvanceVirtual(uint32_t frameCount) noexcept
	{
		if (m_State != VoiceState::Virtual) return m_State != VoiceState::Free;
		if (!m_Buffer) { m_State = VoiceState::Free; return false; }

		uint32_t bufFrames = m_Buffer->GetFrameCount();
		if (bufFrames == 0) { m_State = VoiceState::Free; return false; }

		double step = static_cast<double>(m_Pitch);
		m_CursorFrame += step * static_cast<double>(frameCount);

		if (m_CursorFrame >= static_cast<double>(bufFrames))
		{
			if (m_Looping)
			{
				m_CursorFrame = std::fmod(m_CursorFrame, static_cast<double>(bufFrames));
			}
			else
			{
				m_State = VoiceState::Free;
				return false;
			}
		}

		return true;
	}

	float Voice::CalculateStealScore() const noexcept
	{
		if (m_State == VoiceState::Free) return 0.0f;
		float timeRem = 100.0f;
		if (m_Buffer && m_Buffer->GetFrameCount() > 0)
		{
			timeRem = static_cast<float>(m_Buffer->GetFrameCount() - static_cast<uint32_t>(m_CursorFrame)) / 48000.0f;
		}
		return static_cast<float>(m_Priority) * 1000.0f + m_CurrentAudibility * 100.0f + timeRem * 10.0f;
	}
}
