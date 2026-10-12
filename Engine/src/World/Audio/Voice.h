#pragma once

#include "AudioTypes.h"
#include "AudioBuffer.h"
#include "AudioDsp.h"
#include <memory>
#include <vector>

namespace World::Audio
{
	class WLD_API Voice
	{
	public:
		Voice() = default;

		void Reset() noexcept;

		bool IsActive() const noexcept { return m_State != VoiceState::Free; }
		bool IsPlaying() const noexcept { return m_State == VoiceState::Playing; }
		bool IsVirtual() const noexcept { return m_State == VoiceState::Virtual; }

		void Setup(
			VoiceHandle handle,
			const std::shared_ptr<AudioBuffer>& buffer,
			BusId busId,
			float volume = 1.0f,
			float pitch = 1.0f,
			bool loop = false,
			uint8_t priority = 128) noexcept;

		void Stop(float fadeOutSeconds = 0.05f) noexcept;
		void Pause() noexcept;
		void Resume() noexcept;

		void SetSpatialParameters(
			const glm::vec3& position,
			const glm::vec3& forward,
			float minDistance,
			float maxDistance,
			AttenuationModel model,
			float innerAngle = 360.0f,
			float outerAngle = 360.0f,
			float outerGain = 1.0f) noexcept;

		void SetOcclusion(float occlusion01) noexcept;
		void SetPitch(float pitch) noexcept;
		void SetVolume(float volume) noexcept;
		void SetLooping(bool loop) noexcept { m_Looping = loop; }
		bool IsLooping() const noexcept { return m_Looping; }

		void UpdateSpatial(
			const glm::vec3& listenerPos,
			const glm::vec3& listenerRight,
			float sampleRate) noexcept;

		// Renders stereo frames into outLeft/outRight buffers; returns true if still playing
		bool RenderBlock(
			float* outLeft,
			float* outRight,
			uint32_t frameCount,
			float sampleRate) noexcept;

		// Advances play cursor without rendering (for virtual voices)
		bool AdvanceVirtual(uint32_t frameCount) noexcept;

		float CalculateStealScore() const noexcept;

		VoiceHandle GetHandle() const noexcept { return m_Handle; }
		BusId GetBusId() const noexcept { return m_BusId; }
		VoiceState GetState() const noexcept { return m_State; }
		uint8_t GetPriority() const noexcept { return m_Priority; }
		float GetAudibility() const noexcept { return m_CurrentAudibility; }

	private:
		VoiceHandle m_Handle { kInvalidVoiceHandle };
		BusId m_BusId { 0 };
		VoiceState m_State { VoiceState::Free };
		std::shared_ptr<AudioBuffer> m_Buffer;

		double m_CursorFrame { 0.0 };
		float m_BaseVolume { 1.0f };
		float m_Pitch { 1.0f };
		bool m_Looping { false };
		uint8_t m_Priority { 128 };

		// Spatial 3D
		bool m_IsSpatial { false };
		glm::vec3 m_Position { 0.0f };
		glm::vec3 m_Forward { 0.0f, 0.0f, -1.0f };
		float m_MinDistance { 1.0f };
		float m_MaxDistance { 25.0f };
		AttenuationModel m_AttenuationModel { AttenuationModel::InverseSquare };
		float m_InnerAngle { 360.0f };
		float m_OuterAngle { 360.0f };
		float m_OuterGain { 1.0f };

		// Gains & DSP
		float m_LeftGain { 0.7071f };
		float m_RightGain { 0.7071f };
		float m_CurrentAudibility { 1.0f };
		float m_Occlusion { 0.0f };
		BiquadFilter m_FilterLeft;
		BiquadFilter m_FilterRight;

		// Fade out
		uint32_t m_FadeRemainingFrames { 0 };
		uint32_t m_FadeTotalFrames { 0 };
	};
}
