#include "wldpch.h"
#include "OpenGLUniformBufferSet.h"

namespace World
{
	OpenGLUniformBufferSet::OpenGLUniformBufferSet(uint32_t binding, int32_t size, uint32_t framesInFlight)
		: m_FramesInFlight(framesInFlight)
	{
		for (uint32_t i = 0; i < m_FramesInFlight; i++)
		{
			// 为每一帧创建一个物理上的 UniformBuffer
			m_UniformBuffers[i] = UniformBuffer::Create(size, binding);
		}
	}

	void OpenGLUniformBufferSet::SetData(const void* data, uint32_t size, uint32_t offset)
	{
		for (auto& [frame, ubo] : m_UniformBuffers)
		{
			ubo->SetData(data, size, offset);
		}
	}


}