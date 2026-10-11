#pragma once

#include "AudioTypes.h"
#include "Voice.h"
#include <array>
#include <unordered_map>
#include <mutex>

namespace World::Audio
{
	class WLD_API VoicePool
	{
	public:
		VoicePool();

		void Reset();

		VoiceHandle AllocateVoice(
			const std::shared_ptr<AudioBuffer>& buffer,
			BusId busId,
			float volume = 1.0f,
			float pitch = 1.0f,
			bool loop = false,
			uint8_t priority = 128,
			StealPolicy stealPolicy = StealPolicy::StealOldest);

		void StopVoice(VoiceHandle handle, float fadeOutSeconds = 0.05f);
		void PauseVoice(VoiceHandle handle);
		void ResumeVoice(VoiceHandle handle);
		void StopAllVoices(float fadeOutSeconds = 0.05f);

		Voice* FindVoice(VoiceHandle handle);

		void UpdateSpatialAll(
			const glm::vec3& listenerPos,
			const glm::vec3& listenerRight,
			float sampleRate);

		// Called exclusively from Audio Mixer thread
		void MixVoicesIntoBuses(
			std::unordered_map<BusId, std::pair<std::vector<float>, std::vector<float>>>& busBuffers,
			uint32_t frameCount,
			float sampleRate);

		uint32_t GetActivePhysicalVoiceCount() const noexcept;
		uint32_t GetActiveVirtualVoiceCount() const noexcept;

	private:
		VoiceHandle m_NextHandle { 1 };
		std::array<Voice, kMaxPhysicalVoices> m_PhysicalVoices;
		std::array<Voice, kMaxVirtualVoices> m_VirtualVoices;
	};
}
