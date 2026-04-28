#pragma once
#include "World/Renderer/Shader.h"

namespace World
{
	struct PipelineSpecification
	{
		//Ref<RenderPass> RenderPass;
		Ref<Shader> Shader;

		BufferLayout Layout;

		bool BackfaceCulling = true;
		bool DepthTest = true;
		bool DepthWrite = true;
		bool Wireframe = false;
		float LineWidth = 1.0f;

		// 多重采样 (MSAA)
		//VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;

		std::string DebugName;
	};

	class PipelineStateObject
	{
	public:
		static Ref<PipelineStateObject> Create(const PipelineSpecification& spec);
	public:
		virtual ~PipelineStateObject() = default;

		virtual void Bind() = 0;
		virtual void Unbind() = 0;

		virtual const PipelineSpecification& GetSpecification() const = 0;

	};
}