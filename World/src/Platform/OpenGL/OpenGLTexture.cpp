#include "wldpch.h"
#include "OpenGLTexture.h"
#include "World/Core/Application.h"

#include "World/Renderer/TextureData.h"

namespace World
{
	OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
		: m_Path(path)
	{
		WLD_PROFILE_FUNCTION();

		// D3:查找/兜底逻辑抽到 TextureData(与 RHI 原生上传共用同一份策略)。
		// flipVertically=true 保持 GL 纹理的历史朝向(UV (0,0) 在左下)。
		const TextureData textureData = LoadTextureData(m_Path, /*flipVertically*/ true);
		const uint32_t width = textureData.Width;
		const uint32_t height = textureData.Height;
		const int channels = textureData.Channels;
		const uint8_t* data = textureData.Pixels.data();
		GLint internalFormat = 0, dataFormat = 0;
		if (channels == 4 || channels == 2)
		{
			internalFormat = GL_RGBA8;
			dataFormat = GL_RGBA;
		}
		else if (channels == 3)
		{
			internalFormat = GL_RGB8;
			dataFormat = GL_RGB;
		}
		m_InternalFormat = internalFormat;
		m_DataFormat = dataFormat;

		m_Width = width;
		m_Height = height;

		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);

		glTextureStorage2D(m_RendererID, 1, m_InternalFormat, m_Width, m_Height);

		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

		glTextureSubImage2D(m_RendererID, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, data);
		// 像素缓冲归 TextureData 所有(RHI 化后不再由 stb 分配,无需 stbi_image_free)。
	}
	OpenGLTexture2D::OpenGLTexture2D(uint32_t width, uint32_t height)
		: m_Width(width), m_Height(height)
	{
		WLD_PROFILE_FUNCTION();

		m_InternalFormat = GL_RGBA8;
		m_DataFormat = GL_RGBA;

		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);

		glTextureStorage2D(m_RendererID, 1, m_InternalFormat, m_Width, m_Height);

		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
	OpenGLTexture2D::~OpenGLTexture2D()
	{
		WLD_PROFILE_FUNCTION();

		glDeleteTextures(1, &m_RendererID);
	}
	void OpenGLTexture2D::Bind(uint32_t slot) const
	{
		WLD_PROFILE_FUNCTION();

		glBindTextureUnit(slot, m_RendererID);
	}
	void OpenGLTexture2D::SetData(void* data, uint32_t size)
	{
		WLD_PROFILE_FUNCTION();
		// TODO: Add support for other data formats (e.g. RGBA16F)
		WLD_ASSERT(size == m_Width * m_Height * (m_DataFormat == GL_RGBA ? 4 : 3), "Data must be entire texture!");
		glTextureSubImage2D(m_RendererID, 0, 0, 0, m_Width, m_Height, m_DataFormat, GL_UNSIGNED_BYTE, data);
	}
}
