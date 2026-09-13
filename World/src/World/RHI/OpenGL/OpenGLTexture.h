#pragma once

#include "World/RHI/RhiTexture.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	class OpenGLTexture : public Texture
	{
	public:
		explicit OpenGLTexture(const TextureDesc& desc);
		~OpenGLTexture() override;

		const TextureDesc& GetDesc() const override { return m_Desc; }
		void SetData(const void* data, uint64_t size, uint32_t layer = 0, uint32_t mip = 0) override;

		GLuint GetID() const { return m_ID; }

	private:
		TextureDesc m_Desc;
		GLuint m_ID = 0;
	};
}
