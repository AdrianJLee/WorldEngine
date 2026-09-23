#pragma once
#include "World/Renderer/UniformBufferSet.h"
#include <map>
namespace World
{
	class OpenGLUniformBufferSet : public UniformBufferSet
	{
	public:
		OpenGLUniformBufferSet(uint32_t binding, int32_t size, uint32_t framesInFlight);
		virtual ~OpenGLUniformBufferSet() override = default;

		virtual Ref<UniformBuffer> Get(uint32_t frame) override { return m_UniformBuffers[frame]; }

		virtual void SetData(const void* data, uint32_t size, uint32_t offset = 0) override;
	private:
		uint32_t m_FramesInFlight;

		// <frameIndex, UniformBuffer>
		std::map<uint32_t, Ref<UniformBuffer>> m_UniformBuffers;
	};
}