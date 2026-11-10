#pragma once

#include "AudioTypes.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace World::Audio
{
	class AudioSystem;

	class WLD_API SplineAudioEmitter
	{
	public:
		SplineAudioEmitter() = default;
		explicit SplineAudioEmitter(std::vector<glm::vec3> points, bool closedLoop = false);
		~SplineAudioEmitter() = default;

		void SetPoints(std::vector<glm::vec3> points, bool closedLoop = false);
		const std::vector<glm::vec3>& GetPoints() const noexcept { return m_Points; }
		bool IsClosedLoop() const noexcept { return m_ClosedLoop; }
		void SetClosedLoop(bool closed) noexcept { m_ClosedLoop = closed; }

		void AddPoint(const glm::vec3& point);
		void ClearPoints() noexcept;
		size_t GetPointCount() const noexcept { return m_Points.size(); }

		// Core geometry algorithm: finds closest point on polyline/spline to listenerPos
		glm::vec3 GetClosestPointOnSpline(const glm::vec3& listenerPos) const noexcept;

		// Distance from listener to closest point on spline
		float GetDistanceToSpline(const glm::vec3& listenerPos) const noexcept;

		// Calculate adaptive volumetric spread [0.0, maxSpread] based on proximity
		float CalculateVolumetricSpread(const glm::vec3& listenerPos) const noexcept;

		void SetVolumetricParameters(float innerRadius = 2.0f, float outerRadius = 15.0f, float maxSpread = 1.0f) noexcept;
		float GetInnerRadius() const noexcept { return m_InnerRadius; }
		float GetOuterRadius() const noexcept { return m_OuterRadius; }
		float GetMaxSpread() const noexcept { return m_MaxSpread; }

		// Audio playback integration
		VoiceHandle Play(AudioSystem& audio, const std::string& eventId, float volume = 1.0f, float pitch = 1.0f);
		void Stop(AudioSystem& audio, float fadeSeconds = 0.1f);
		void Update(float deltaSeconds, const glm::vec3& listenerPos, AudioSystem& audio);

		VoiceHandle GetVoiceHandle() const noexcept { return m_VoiceHandle; }
		bool IsPlaying() const noexcept { return m_VoiceHandle != kInvalidVoiceHandle; }
		const glm::vec3& GetLastVirtualPosition() const noexcept { return m_LastVirtualPos; }
		float GetCurrentSpread() const noexcept { return m_CurrentSpread; }

	private:
		std::vector<glm::vec3> m_Points;
		bool m_ClosedLoop { false };
		float m_InnerRadius { 2.0f };   // Distance where spread is maximum
		float m_OuterRadius { 15.0f };  // Distance where spread fades to point source (0.0)
		float m_MaxSpread { 1.0f };

		VoiceHandle m_VoiceHandle { kInvalidVoiceHandle };
		glm::vec3 m_LastVirtualPos { 0.0f };
		float m_CurrentSpread { 0.0f };
	};
}
