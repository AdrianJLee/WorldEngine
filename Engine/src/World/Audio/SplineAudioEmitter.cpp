#include "SplineAudioEmitter.h"
#include "AudioSystem.h"
#include <limits>
#include <cmath>

namespace World::Audio
{
	SplineAudioEmitter::SplineAudioEmitter(std::vector<glm::vec3> points, bool closedLoop)
		: m_Points(std::move(points))
		, m_ClosedLoop(closedLoop)
	{
	}

	void SplineAudioEmitter::SetPoints(std::vector<glm::vec3> points, bool closedLoop)
	{
		m_Points = std::move(points);
		m_ClosedLoop = closedLoop;
	}

	void SplineAudioEmitter::AddPoint(const glm::vec3& point)
	{
		m_Points.push_back(point);
	}

	void SplineAudioEmitter::ClearPoints() noexcept
	{
		m_Points.clear();
	}

	glm::vec3 SplineAudioEmitter::GetClosestPointOnSpline(const glm::vec3& listenerPos) const noexcept
	{
		if (m_Points.empty())
		{
			return listenerPos;
		}

		if (m_Points.size() == 1)
		{
			return m_Points[0];
		}

		const size_t segmentCount = m_ClosedLoop ? m_Points.size() : (m_Points.size() - 1);
		float minDistanceSq = std::numeric_limits<float>::max();
		glm::vec3 bestPoint = m_Points[0];

		for (size_t i = 0; i < segmentCount; ++i)
		{
			const glm::vec3& a = m_Points[i];
			const glm::vec3& b = m_Points[(i + 1) % m_Points.size()];

			glm::vec3 ab = b - a;
			float l2 = glm::dot(ab, ab);
			glm::vec3 candPoint;

			if (l2 < 1e-7f)
			{
				candPoint = a;
			}
			else
			{
				float t = glm::dot(listenerPos - a, ab) / l2;
				t = std::clamp(t, 0.0f, 1.0f);
				candPoint = a + t * ab;
			}

			glm::vec3 diff = listenerPos - candPoint;
			float distSq = glm::dot(diff, diff);
			if (distSq < minDistanceSq)
			{
				minDistanceSq = distSq;
				bestPoint = candPoint;
			}
		}

		return bestPoint;
	}

	float SplineAudioEmitter::GetDistanceToSpline(const glm::vec3& listenerPos) const noexcept
	{
		glm::vec3 closest = GetClosestPointOnSpline(listenerPos);
		return glm::distance(listenerPos, closest);
	}

	float SplineAudioEmitter::CalculateVolumetricSpread(const glm::vec3& listenerPos) const noexcept
	{
		float dist = GetDistanceToSpline(listenerPos);
		if (dist <= m_InnerRadius)
		{
			return m_MaxSpread;
		}
		if (dist >= m_OuterRadius)
		{
			return 0.0f;
		}

		float t = (dist - m_InnerRadius) / (m_OuterRadius - m_InnerRadius);
		return (1.0f - t) * m_MaxSpread;
	}

	void SplineAudioEmitter::SetVolumetricParameters(float innerRadius, float outerRadius, float maxSpread) noexcept
	{
		m_InnerRadius = std::max(0.1f, innerRadius);
		m_OuterRadius = std::max(m_InnerRadius + 0.1f, outerRadius);
		m_MaxSpread = std::clamp(maxSpread, 0.0f, 1.0f);
	}

	VoiceHandle SplineAudioEmitter::Play(AudioSystem& audio, const std::string& eventId, float volume, float pitch)
	{
		glm::vec3 listenerPos = audio.GetListenerPosition();
		m_LastVirtualPos = GetClosestPointOnSpline(listenerPos);
		m_CurrentSpread = CalculateVolumetricSpread(listenerPos);

		m_VoiceHandle = audio.PostEvent(eventId, m_LastVirtualPos, volume, pitch, true);
		if (m_VoiceHandle != kInvalidVoiceHandle)
		{
			audio.SetVoiceVolumetricRadius(m_VoiceHandle, m_CurrentSpread * m_InnerRadius);
		}
		return m_VoiceHandle;
	}

	void SplineAudioEmitter::Stop(AudioSystem& audio, float fadeSeconds)
	{
		if (m_VoiceHandle != kInvalidVoiceHandle)
		{
			audio.StopVoice(m_VoiceHandle, fadeSeconds);
			m_VoiceHandle = kInvalidVoiceHandle;
		}
	}

	void SplineAudioEmitter::Update(float deltaSeconds, const glm::vec3& listenerPos, AudioSystem& audio)
	{
		(void)deltaSeconds;
		if (m_VoiceHandle == kInvalidVoiceHandle)
		{
			return;
		}

		m_LastVirtualPos = GetClosestPointOnSpline(listenerPos);
		m_CurrentSpread = CalculateVolumetricSpread(listenerPos);

		audio.SetVoiceVolumetricRadius(m_VoiceHandle, m_CurrentSpread * m_InnerRadius);

		AudioCommand cmd;
		cmd.type = AudioCommandType::SetVoiceSpatialPosition;
		cmd.voiceHandle = m_VoiceHandle;
		cmd.vecParam = m_LastVirtualPos;
		audio.EnqueueCommand(cmd);
	}
}
