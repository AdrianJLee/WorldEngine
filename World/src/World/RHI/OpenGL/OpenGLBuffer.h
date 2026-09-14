#pragma once

#include "World/RHI/RhiBuffer.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	class OpenGLBuffer : public Buffer
	{
	public:
		explicit OpenGLBuffer(const BufferDesc& desc);
		~OpenGLBuffer() override;

		const BufferDesc& GetDesc() const override { return m_Desc; }
		void* Map(uint64_t offset = 0, uint64_t size = 0) override;
		void Unmap() override;
		void SetData(const void* data, uint64_t size, uint64_t offset = 0) override;

		GLuint GetID() const { return m_ID; }
		GLenum GetTarget() const { return m_Target; }

	private:
		BufferDesc m_Desc;
		GLuint m_ID = 0;
		GLenum m_Target = GL_ARRAY_BUFFER;
	};
}
