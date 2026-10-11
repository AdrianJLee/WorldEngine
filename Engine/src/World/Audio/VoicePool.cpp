#include "VoicePool.h"

namespace World::Audio
{
	VoicePool::VoicePool()
	{
		Reset();
	}

	void VoicePool::Reset()
	{
		for (auto& v : m_PhysicalVoices) v.Reset();
		for (auto& v : m_VirtualVoices) v.Reset();
		m_NextHandle = 1;
	}

	VoiceHandle VoicePool::AllocateVoice(
		const std::shared_ptr<AudioBuffer>& buffer,
		BusId busId,
		float volume,
		float pitch,
		bool loop,
		uint8_t priority,
		StealPolicy stealPolicy)
	{
		if (!buffer || buffer->GetFrameCount() == 0) return kInvalidVoiceHandle;

		VoiceHandle handle = m_NextHandle++;
		if (m_NextHandle == kInvalidVoiceHandle) m_NextHandle = 1;

		// 1. Search for a free physical voice slot
		for (auto& v : m_PhysicalVoices)
		{
			if (!v.IsActive())
			{
				v.Setup(handle, buffer, busId, volume, pitch, loop, priority);
				return handle;
			}
		}

		// 2. Physical voices are full! Apply stealing policy
		if (stealPolicy != StealPolicy::StealReject)
		{
			size_t victimIndex = 0;
			float lowestScore = 1e9f;

			for (size_t i = 0; i < m_PhysicalVoices.size(); ++i)
			{
				float score = m_PhysicalVoices[i].CalculateStealScore();
				if (score < lowestScore)
				{
					lowestScore = score;
					victimIndex = i;
				}
			}

			// Steal victim: allocate new voice into its slot
			m_PhysicalVoices[victimIndex].Setup(handle, buffer, busId, volume, pitch, loop, priority);
			return handle;
		}

		// 3. Fallback: Allocate into virtual voice tracker
		for (auto& v : m_VirtualVoices)
		{
			if (!v.IsActive())
			{
				v.Setup(handle, buffer, busId, volume, pitch, loop, priority);
				return handle;
			}
		}

		return kInvalidVoiceHandle;
	}

	void VoicePool::StopVoice(VoiceHandle handle, float fadeOutSeconds)
	{
		if (auto* v = FindVoice(handle))
		{
			v->Stop(fadeOutSeconds);
		}
	}

	void VoicePool::PauseVoice(VoiceHandle handle)
	{
		if (auto* v = FindVoice(handle))
		{
			v->Pause();
		}
	}

	void VoicePool::ResumeVoice(VoiceHandle handle)
	{
		if (auto* v = FindVoice(handle))
		{
			v->Resume();
		}
	}

	void VoicePool::StopAllVoices(float fadeOutSeconds)
	{
		for (auto& v : m_PhysicalVoices) v.Stop(fadeOutSeconds);
		for (auto& v : m_VirtualVoices) v.Stop(fadeOutSeconds);
	}

	Voice* VoicePool::FindVoice(VoiceHandle handle)
	{
		if (handle == kInvalidVoiceHandle) return nullptr;
		for (auto& v : m_PhysicalVoices)
		{
			if (v.GetHandle() == handle) return &v;
		}
		for (auto& v : m_VirtualVoices)
		{
			if (v.GetHandle() == handle) return &v;
		}
		return nullptr;
	}

	void VoicePool::UpdateSpatialAll(
		const glm::vec3& listenerPos,
		const glm::vec3& listenerRight,
		float sampleRate)
	{
		for (auto& v : m_PhysicalVoices)
		{
			if (v.IsActive()) v.UpdateSpatial(listenerPos, listenerRight, sampleRate);
		}
		for (auto& v : m_VirtualVoices)
		{
			if (v.IsActive()) v.UpdateSpatial(listenerPos, listenerRight, sampleRate);
		}
	}

	void VoicePool::MixVoicesIntoBuses(
		std::unordered_map<BusId, std::pair<std::vector<float>, std::vector<float>>>& busBuffers,
		uint32_t frameCount,
		float sampleRate)
	{
		// Render physical voices
		for (auto& v : m_PhysicalVoices)
		{
			if (v.IsPlaying() || v.GetState() == VoiceState::Stopping)
			{
				BusId bid = v.GetBusId();
				auto& entry = busBuffers[bid];
				if (entry.first.size() < frameCount)
				{
					entry.first.assign(frameCount, 0.0f);
					entry.second.assign(frameCount, 0.0f);
				}

				v.RenderBlock(entry.first.data(), entry.second.data(), frameCount, sampleRate);
			}
		}

		// Advance virtual voices without DSP cost
		for (auto& v : m_VirtualVoices)
		{
			if (v.IsVirtual())
			{
				v.AdvanceVirtual(frameCount);
			}
		}
	}

	uint32_t VoicePool::GetActivePhysicalVoiceCount() const noexcept
	{
		uint32_t count = 0;
		for (const auto& v : m_PhysicalVoices)
		{
			if (v.IsActive()) ++count;
		}
		return count;
	}

	uint32_t VoicePool::GetActiveVirtualVoiceCount() const noexcept
	{
		uint32_t count = 0;
		for (const auto& v : m_VirtualVoices)
		{
			if (v.IsActive()) ++count;
		}
		return count;
	}
}
