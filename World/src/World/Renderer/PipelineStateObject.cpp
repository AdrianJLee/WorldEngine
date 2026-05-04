#include "wldpch.h"
#include "PipelineStateObject.h"
#include "World/Renderer/RendererAPI.h"
#include "Platform/OpenGL/OpenGLPipeline.h"
namespace World
{
	Ref<PipelineStateObject> PipelineStateObject::Create(const PipelineSpecification& spec)
	{
		switch (RendererAPI::GetAPI())
		{
			case RendererAPI::API::None:
				WLD_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return nullptr;
			case RendererAPI::API::OpenGL:
				return CreateRef<OpenGLPipeline>(spec);
			case RendererAPI::API::Vulkan:
				//return CreateRef<VulkanPipeline>(VulkanContext::GetCurrent()->GetDevice(), spec);
				WLD_CORE_ASSERT(false, "Vulkan Pipeline is not implemented yet!");
				return nullptr;
			default:
				WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
				return nullptr;
		}
	}
}