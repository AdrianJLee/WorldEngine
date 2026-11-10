#pragma once

#include "World/RHI/RhiShader.h"

namespace World::Rhi::OpenGL
{
	class OpenGLShader : public Shader
	{
	public:
		explicit OpenGLShader(const ShaderDesc& desc) : m_Desc(desc) {}
		const ShaderDesc& GetDesc() const override { return m_Desc; }

	private:
		ShaderDesc m_Desc;
	};
}
