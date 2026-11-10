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
		// 包装已有 GL 纹理(如旧 Texture2D),不取得所有权。
		void Adopt(GLuint id) { m_ID = id; m_OwnsId = false; }

	private:
		TextureDesc m_Desc;
		GLuint m_ID = 0;
		bool m_OwnsId = true;
	};
}
