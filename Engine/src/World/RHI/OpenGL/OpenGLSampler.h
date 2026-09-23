#pragma once

#include "World/RHI/RhiSampler.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	class OpenGLSampler : public Sampler
	{
	public:
		explicit OpenGLSampler(const SamplerDesc& desc);
		~OpenGLSampler() override;

		const SamplerDesc& GetDesc() const override { return m_Desc; }
		GLuint GetID() const { return m_ID; }

	private:
		SamplerDesc m_Desc;
		GLuint m_ID = 0;
	};
}
