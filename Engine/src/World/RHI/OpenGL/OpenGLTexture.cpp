#include "wldpch.h"
#include "OpenGLTexture.h"
#include "OpenGLHelpers.h"

namespace World::Rhi::OpenGL
{
	namespace
	{
		// 块压缩格式:上传走 glCompressedTextureSubImage*(ToGLDataFormat/ToGLDataType
		// 对它们没有映射 —— 旧路径会断言,这就是 M4-TEX P2 要修的那条)。
		bool IsBlockCompressedFormat(Format format)
		{
			switch (format)
			{
				case Format::BC1_UNORM:
				case Format::BC2_UNORM:
				case Format::BC3_UNORM:
				case Format::BC4_UNORM:
				case Format::BC5_UNORM:
				case Format::BC7_UNORM:
				case Format::BC1_UNORM_SRGB:
				case Format::BC3_UNORM_SRGB:
				case Format::BC7_UNORM_SRGB:
					return true;
				default:
					return false;
			}
		}

		uint32_t BlockSizeBytes(Format format)
		{
			switch (format)
			{
				case Format::BC1_UNORM:
				case Format::BC1_UNORM_SRGB:
				case Format::BC4_UNORM:
					return 8;
				case Format::BC2_UNORM:
				case Format::BC3_UNORM:
				case Format::BC3_UNORM_SRGB:
				case Format::BC5_UNORM:
				case Format::BC7_UNORM:
				case Format::BC7_UNORM_SRGB:
					return 16;
				default:
					return 0;
			}
		}
	}

	OpenGLTexture::OpenGLTexture(const TextureDesc& desc)
		: m_Desc(desc)
	{
		const GLenum internalFormat = ToGLInternalFormat(desc.Format);
		// P4-4a:多采样存储。GL 只有 2D 一种多采样纹理目标(GL_TEXTURE_2D_MULTISAMPLE),
		// 1D/3D/Cube 没有对应的多采样存储形式 —— 那些类型保持原来的单采样路径。
		const bool wantsMultisample = desc.Samples != SampleCount::Count1;
		const bool multisampled = wantsMultisample && desc.Type == TextureType::Texture2D;
		if (wantsMultisample && !multisampled)
			WLD_CORE_WARN("Multisample storage is only available for 2D textures; texture '{0}' is created "
				"single-sampled (Samples={1} ignored)", desc.DebugName,
				static_cast<uint32_t>(desc.Samples));

		const GLenum target = multisampled ? GL_TEXTURE_2D_MULTISAMPLE : ToGLTextureTarget(desc.Type);
		WLD_CORE_ASSERT(target != 0 && internalFormat != 0, "Unsupported texture type/format in OpenGL backend");

		glCreateTextures(target, 1, &m_ID);
		if (multisampled)
		{
			// 多采样纹理没有 mip 链(levels 恒为 1)。fixedsamplelocations=GL_FALSE:
			// 采样位置不固定,驱动可自由选择(与旧 Platform/OpenGL/OpenGLFramebuffer 的 MSAA 路径一致)。
			glTextureStorage2DMultisample(m_ID, static_cast<GLsizei>(static_cast<uint32_t>(desc.Samples)),
				internalFormat, static_cast<GLsizei>(desc.Extent.Width), static_cast<GLsizei>(desc.Extent.Height),
				GL_FALSE);
		}
		else
		{
			const GLsizei levels = static_cast<GLsizei>(std::max(1u, desc.MipLevels));

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
		// 诊断(WLD_GL_TRACE_DRAW):把纹理 id/格式/尺寸记下来,便于和 FBO 附件、读回 id 对上号。
		static const bool traceTextures = std::getenv("WLD_GL_TRACE_DRAW") != nullptr;
		if (traceTextures)
			WLD_CORE_INFO("[gl-tex] id={0} name='{1}' format={2} extent={3}x{4} samples={5} ctx={6}",
				m_ID, desc.DebugName, static_cast<int>(desc.Format), desc.Extent.Width, desc.Extent.Height,
				static_cast<uint32_t>(desc.Samples),
				reinterpret_cast<uintptr_t>(wglGetCurrentContext()));
	}

	OpenGLTexture::~OpenGLTexture()
	{
		if (m_ID && m_OwnsId)
			glDeleteTextures(1, &m_ID);
	}

	void OpenGLTexture::SetData(const void* data, uint64_t size, uint32_t layer, uint32_t mip)
	{
		// 多采样纹理不能上传/写入(glTextureSubImage* 对它一律 GL_INVALID_OPERATION):
		// 内容只能由绘制写入,再经 resolve 到单采样纹理后读回/采样。
		if (m_Desc.Samples != SampleCount::Count1)
		{
			WLD_CORE_WARN("SetData ignored for multisample texture '{0}' (Samples={1}); "
				"multisample attachments are GPU-written and read back through ResolveTexture",
				m_Desc.DebugName, static_cast<uint32_t>(m_Desc.Samples));
			return;
		}

		const auto levelExtent = [&]()
		{
			return Extent3D{ std::max(1u, m_Desc.Extent.Width >> mip),
				std::max(1u, m_Desc.Extent.Height >> mip),
				std::max(1u, m_Desc.Extent.Depth >> mip) };
		};

		// 块压缩 = 一段 blocks 直接上传:`format` 用与 glTextureStorage2D 相同的内部格式
		// (GL 的 glCompressedTextureSubImage* 要求 format 是压缩格式枚举),imageSize 用
		// 调用方给的字节数(产物头算出来的 ceil(w/4)*ceil(h/4)*blockBytes)。
		if (IsBlockCompressedFormat(m_Desc.Format))
		{
			const GLenum compressedFormat = ToGLInternalFormat(m_Desc.Format);
			WLD_CORE_ASSERT(compressedFormat != 0 && size > 0,
				"Block-compressed texture upload needs a known GL format and non-zero size");
			const auto extent = levelExtent();
			const GLsizei imageSize = static_cast<GLsizei>(size);
			switch (m_Desc.Type)
			{
				case TextureType::Texture1D:
					glCompressedTextureSubImage1D(m_ID, mip, 0, static_cast<GLsizei>(extent.Width),
						compressedFormat, imageSize, data);
					break;
				case TextureType::Texture2D:
					glCompressedTextureSubImage2D(m_ID, mip, 0, 0, static_cast<GLsizei>(extent.Width),
						static_cast<GLsizei>(extent.Height), compressedFormat, imageSize, data);
					break;
				case TextureType::Cube:
				{
					// 每面字节数按块对齐算(旧 RGBA8 分支的 *4 对块格式不成立)。
					const uint64_t faceSize = static_cast<uint64_t>((extent.Width + 3) / 4)
						* ((extent.Height + 3) / 4) * BlockSizeBytes(m_Desc.Format);
					for (uint32_t face = 0; face < 6; face++)
						glCompressedTextureSubImage3D(m_ID, mip, 0, 0, static_cast<GLint>(face),
							static_cast<GLsizei>(extent.Width), static_cast<GLsizei>(extent.Height), 1,
							compressedFormat, static_cast<GLsizei>(faceSize),
							static_cast<const uint8_t*>(data) + face * faceSize);
					break;
				}
				case TextureType::Texture3D:
					glCompressedTextureSubImage3D(m_ID, mip, 0, 0, 0, static_cast<GLsizei>(extent.Width),
						static_cast<GLsizei>(extent.Height), static_cast<GLsizei>(extent.Depth),
						compressedFormat, imageSize, data);
					break;
			}
			return;
		}

		const GLenum dataFormat = ToGLDataFormat(m_Desc.Format);
		const GLenum dataType = ToGLDataType(m_Desc.Format);
		WLD_CORE_ASSERT(dataFormat != 0 && dataType != 0, "Texture format has no upload mapping in OpenGL backend");

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
