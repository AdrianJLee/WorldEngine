#include "wldpch.h"
#include "OpenGLTexture.h"
#include "World/Core/Application.h"

#include <stb_image.h>

namespace World
{
	OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
		: m_Path(path)
	{
		WLD_PROFILE_FUNCTION();

		stbi_set_flip_vertically_on_load(1);

		int width = 1, height = 1, channels = 4;
		stbi_uc* data = nullptr;
		{
			WLD_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::OpenGLTexture2D(const std::string&)");
			// 1. VFS 优先:开发目录 provider 或发行包 provider。
			if (Application::HasInstance())
			{
				std::error_code vfsEc;
				std::vector<uint8_t> bytes;
				if (Application::Get().GetContext().Vfs().Read(m_Path, bytes, vfsEc) && !bytes.empty())
					data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
						&width, &height, &channels, 0);
			}
			// 2. 磁盘回退(未挂载 VFS 的开发环境)。
			if (!data)
			{
				std::string diskPath;
				if (std::filesystem::exists(std::string(WLD_GAME_DIR) + m_Path))
					diskPath = std::string(WLD_GAME_DIR) + m_Path;
				else
					diskPath = std::string(WLD_EDITOR_DIR) + m_Path;
				data = stbi_load(diskPath.c_str(), &width, &height, &channels, 0);
			}
			// 3. 加载失败给 1x1 白色兜底,不让坏资产拖垮进程。
			if (!data)
			{
				WLD_CORE_WARN("Failed to load image '{0}'; using 1x1 white fallback", m_Path);
				static const stbi_uc white[4] = { 255, 255, 255, 255 };
				data = stbi_load_from_memory(white, 4, &width, &height, &channels, 4);
				width = height = 1;
			}
		}
		GLint internalFormat = 0, dataFormat = 0;
		if (channels == 4)
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

		stbi_image_free(data);
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
