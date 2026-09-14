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
		(void)offset;
		(void)size;
		return nullptr;
	}

	void OpenGLBuffer::Unmap()
	{
		// 非映射实现:SetData 由驱动同步,无需 Unmap。
	}

	void OpenGLBuffer::SetData(const void* data, uint64_t size, uint64_t offset)
	{
		glNamedBufferSubData(m_ID, offset, size, data);
	}
}
