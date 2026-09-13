#include "wldpch.h"
#include "OpenGLTexture.h"
#include "OpenGLHelpers.h"

namespace World::Rhi::OpenGL
{
	OpenGLTexture::OpenGLTexture(const TextureDesc& desc)
		: m_Desc(desc)
	{
		const GLenum target = ToGLTextureTarget(desc.Type);
		const GLenum internalFormat = ToGLInternalFormat(desc.Format);
		WLD_CORE_ASSERT(target != 0 && internalFormat != 0, "Unsupported texture type/format in OpenGL backend");

		glCreateTextures(target, 1, &m_ID);
		const GLsizei levels = static_cast<GLsizei>(std::max(1u, desc.MipLevels));
		const GLsizei layers = desc.Type == TextureType::Cube
			? static_cast<GLsizei>(std::max(1u, desc.ArrayLayers) * 6)
			: static_cast<GLsizei>(std::max(1u, desc.ArrayLayers));

		switch (desc.Type)
		{
			case TextureType::Texture1D:
				glTextureStorage1D(m_ID, levels, internalFormat, static_cast<GLsizei>(desc.Extent.Width));
				break;
			case TextureType::Texture2D:
			case TextureType::Cube:
				glTextureStorage2D(m_ID, levels, internalFormat,
					static_cast<GLsizei>(desc.Extent.Width), static_cast<GLsizei>(desc.Extent.Height));
				break;
			case TextureType::Texture3D:
				glTextureStorage3D(m_ID, levels, internalFormat,
					static_cast<GLsizei>(desc.Extent.Width), static_cast<GLsizei>(desc.Extent.Height),
					static_cast<GLsizei>(desc.Extent.Depth));
				break;
		}
	}

	OpenGLTexture::~OpenGLTexture()
	{
		if (m_ID && m_OwnsId)
			glDeleteTextures(1, &m_ID);
	}

	void OpenGLTexture::SetData(const void* data, uint64_t /*size*/, uint32_t layer, uint32_t mip)
	{
		const GLenum dataFormat = ToGLDataFormat(m_Desc.Format);
		const GLenum dataType = ToGLDataType(m_Desc.Format);
		WLD_CORE_ASSERT(dataFormat != 0 && dataType != 0, "Texture format has no upload mapping in OpenGL backend");

		const auto levelExtent = [&]()
		{
			return Extent3D{ std::max(1u, m_Desc.Extent.Width >> mip),
				std::max(1u, m_Desc.Extent.Height >> mip),
				std::max(1u, m_Desc.Extent.Depth >> mip) };
		};

		switch (m_Desc.Type)
		{
			case TextureType::Texture1D:
				glTextureSubImage1D(m_ID, mip, 0, levelExtent().Width, dataFormat, dataType, data);
				break;
			case TextureType::Texture2D:
				glTextureSubImage2D(m_ID, mip, 0, 0, levelExtent().Width, levelExtent().Height, dataFormat, dataType, data);
				break;
			case TextureType::Cube:
			{
				const auto extent = levelExtent();
				for (uint32_t face = 0; face < 6; face++)
					glTextureSubImage3D(m_ID, mip, 0, 0, face, extent.Width, extent.Height, 1, dataFormat, dataType,
						static_cast<const uint8_t*>(data) + static_cast<uint64_t>(face) * extent.Width * extent.Height * 4);
				break;
			}
			case TextureType::Texture3D:
			{
				const auto extent = levelExtent();
				glTextureSubImage3D(m_ID, mip, 0, 0, 0, extent.Width, extent.Height, extent.Depth, dataFormat, dataType, data);
				break;
			}
		}
	}
}
