#include "wldpch.h"
#include "OpenGLPipeline.h"

#include <glad/glad.h>

namespace World
{
	OpenGLPipeline::OpenGLPipeline(const PipelineSpecification& spec)
		:m_PipelineSpec(spec)
	{
		if (m_PipelineSpec.DepthTest)
			glEnable(GL_DEPTH_TEST);
		else
			glDisable(GL_DEPTH_TEST);

		if (m_PipelineSpec.BackfaceCulling)
		{
			glEnable(GL_CULL_FACE);
			glCullFace(GL_BACK);
		}
		else
		{
			glDisable(GL_CULL_FACE);
		}

		if (m_PipelineSpec.Wireframe)
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		else
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
	OpenGLPipeline::~OpenGLPipeline()
	{}
	void OpenGLPipeline::Bind()
	{
		m_PipelineSpec.Shader->Bind();

		// 处理背面剔除等...
		// 注意：在这里我们实际上抹平了 OpenGL 的全局状态机，使其表现得像现代 API 的 Pipeline 一样
	}
	void OpenGLPipeline::Unbind()
	{

	}
}