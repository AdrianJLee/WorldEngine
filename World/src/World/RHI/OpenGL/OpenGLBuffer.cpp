#include "wldpch.h"
#include "OpenGLBuffer.h"
#include "OpenGLHelpers.h"

#include <cstring>

namespace World::Rhi::OpenGL
{
	OpenGLBuffer::OpenGLBuffer(const BufferDesc& desc)
		: m_Desc(desc)
	{
		if (desc.Usage & BufferUsageIndex)
			m_Target = GL_ELEMENT_ARRAY_BUFFER;
		else if (desc.Usage & (BufferUsageStorage | BufferUsageIndirect))
			m_Target = GL_SHADER_STORAGE_BUFFER;
		else
			m_Target = GL_ARRAY_BUFFER;

		GLbitfield storageFlags = GL_DYNAMIC_STORAGE_BIT;
		if (desc.Memory != MemoryHint::DeviceLocal)
			storageFlags |= GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

		glCreateBuffers(1, &m_ID);
		glNamedBufferStorage(m_ID, desc.Size, desc.InitialData, storageFlags);

		if (desc.Memory != MemoryHint::DeviceLocal)
			m_Mapped = static_cast<uint8_t*>(
				glMapNamedBufferRange(m_ID, 0, desc.Size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
	}

	OpenGLBuffer::~OpenGLBuffer()
	{
		if (m_ID)
			glDeleteBuffers(1, &m_ID);
	}

	void* OpenGLBuffer::Map(uint64_t offset, uint64_t size)
	{
		if (!m_Mapped)
			return nullptr;
		return m_Mapped + offset;
	}

	void OpenGLBuffer::Unmap()
	{
		// 持久映射,显式 Unmap 为 no-op。
	}

	void OpenGLBuffer::SetData(const void* data, uint64_t size, uint64_t offset)
	{
		if (m_Mapped)
		{
			std::memcpy(m_Mapped + offset, data, size);
			return;
		}
		glNamedBufferSubData(m_ID, offset, size, data);
	}
}
