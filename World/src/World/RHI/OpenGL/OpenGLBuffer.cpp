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

		// GL_MAP_READ_BIT:读回路径(截图/校验)要能 glMapNamedBufferRange;
		// 持久映射仍不使用(见下方注释),所以这里只开"可读"。
		GLbitfield storageFlags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT;

		glCreateBuffers(1, &m_ID);
		glNamedBufferStorage(m_ID, desc.Size, desc.InitialData, storageFlags);
		// 不使用持久映射:批绘制后端每帧多次覆写同一缓冲区,映射写入与 GPU
		// 读取之间没有同步,早期批次会读到被后续批次覆盖的顶点数据(表现为
		// 界面局部不绘制)。glNamedBufferSubData 由驱动处理读写冲突。
	}

	OpenGLBuffer::~OpenGLBuffer()
	{
		if (m_ID)
			glDeleteBuffers(1, &m_ID);
	}

	void* OpenGLBuffer::Map(uint64_t offset, uint64_t size)
	{
		if (m_Mapped)
			return nullptr;
		const uint64_t length = size != 0 ? size : (m_Desc.Size > offset ? m_Desc.Size - offset : 0);
		if (length == 0)
			return nullptr;
		// DSA 映射(GL 4.5+):读回路径(截图、GPU 数据校验)需要 CPU 侧指针。
		// 注意映射期间不能再对同一 buffer 做 SetData,因此只在读回结束时 Unmap。
		m_Mapped = glMapNamedBufferRange(m_ID, offset, length, GL_MAP_READ_BIT);
		return m_Mapped;
	}

	void OpenGLBuffer::Unmap()
	{
		if (!m_Mapped)
			return;
		glUnmapNamedBuffer(m_ID);
		m_Mapped = nullptr;
	}

	void OpenGLBuffer::SetData(const void* data, uint64_t size, uint64_t offset)
	{
		glNamedBufferSubData(m_ID, offset, size, data);
	}
}
