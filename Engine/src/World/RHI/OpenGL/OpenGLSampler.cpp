#include "wldpch.h"
#include "OpenGLSampler.h"
#include "OpenGLHelpers.h"

#include <glm/gtc/type_ptr.hpp>

namespace World::Rhi::OpenGL
{
	OpenGLSampler::OpenGLSampler(const SamplerDesc& desc)
		: m_Desc(desc)
	{
		glCreateSamplers(1, &m_ID);
		glSamplerParameteri(m_ID, GL_TEXTURE_MIN_FILTER, ToGLMipFilter(desc.MinFilter, desc.MipmapMode));
		glSamplerParameteri(m_ID, GL_TEXTURE_MAG_FILTER, ToGLFilter(desc.MagFilter));
		glSamplerParameteri(m_ID, GL_TEXTURE_WRAP_S, ToGLWrap(desc.AddressU));
		glSamplerParameteri(m_ID, GL_TEXTURE_WRAP_T, ToGLWrap(desc.AddressV));
		glSamplerParameteri(m_ID, GL_TEXTURE_WRAP_R, ToGLWrap(desc.AddressW));
		glSamplerParameterf(m_ID, GL_TEXTURE_LOD_BIAS, desc.MipLodBias);
		glSamplerParameterf(m_ID, GL_TEXTURE_MIN_LOD, desc.MinLod);
		glSamplerParameterf(m_ID, GL_TEXTURE_MAX_LOD, desc.MaxLod);
		glSamplerParameterf(m_ID, GL_TEXTURE_MAX_ANISOTROPY, std::max(1.0f, desc.MaxAnisotropy));
		if (desc.EnableCompare)
		{
			glSamplerParameteri(m_ID, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
			glSamplerParameteri(m_ID, GL_TEXTURE_COMPARE_FUNC, ToGLCompare(desc.Compare));
		}
		if (desc.AddressU == SamplerAddressMode::ClampToBorder ||
			desc.AddressV == SamplerAddressMode::ClampToBorder ||
			desc.AddressW == SamplerAddressMode::ClampToBorder)
			glSamplerParameterfv(m_ID, GL_TEXTURE_BORDER_COLOR, glm::value_ptr(glm::vec4(0.0f)));
	}

	OpenGLSampler::~OpenGLSampler()
	{
		if (m_ID)
			glDeleteSamplers(1, &m_ID);
	}
}
