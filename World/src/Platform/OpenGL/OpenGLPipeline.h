#pragma once
#include "World/Renderer/PipelineStateObject.h"
namespace World
{
	class OpenGLPipeline :public PipelineStateObject
	{
	public:
		OpenGLPipeline(const PipelineSpecification& spec);
		virtual ~OpenGLPipeline();
		OpenGLPipeline(const OpenGLPipeline&) = delete;
		OpenGLPipeline& operator=(const OpenGLPipeline&) = delete;

		virtual void Bind() override;
		virtual void Unbind() override;

		virtual inline const PipelineSpecification& GetSpecification() const { return m_PipelineSpec; };
	private:
		void SetDepthTest(bool enabled);
	private:
		PipelineSpecification m_PipelineSpec;

	};
}