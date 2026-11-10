#pragma once

#include "World/RHI/RhiPipeline.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	class OpenGLPipeline : public Pipeline
	{
	public:
		explicit OpenGLPipeline(const PipelineDesc& desc);
		~OpenGLPipeline() override;

		const PipelineDesc& GetDesc() const override { return m_Desc; }

		void Bind() const;
		GLuint GetProgram() const { return m_Program; }
		GLuint GetVertexArray() const { return m_VertexArray; }

	private:
		PipelineDesc m_Desc;
		GLuint m_Program = 0;
		GLuint m_VertexArray = 0;
	};
}
