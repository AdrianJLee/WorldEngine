#pragma once
#include "RenderPass.h"

#include <glm/glm.hpp>

namespace World
{


	class RendererAPI
	{
	public:
		enum class API
		{
			None = 0,
			OpenGL = 1,
			Vulkan = 2,
		};
	public:
		virtual ~RendererAPI() = default;
		virtual void Init() = 0;
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;

		virtual void DrawIndexed(const Ref<class VertexArray>& vertexArray, uint32_t indexCount) = 0;
		virtual void DrawLines(const Ref<class VertexArray>& vertexArray, uint32_t vertexCount) = 0;
		virtual void SetLineWidth(float width) = 0;
		inline constexpr static API GetAPI() { return s_API; }

		virtual void BeginRenderPass(const Ref<RenderPass>& renderPass, bool clear) = 0;
		virtual void EndRenderPass() = 0;

	private:
		static constexpr API s_API = API::OpenGL;

	};
	namespace RendererConfig
	{
		static constexpr uint32_t MAX_FRAMES_IN_FLIGHT()
		{
			switch (RendererAPI::GetAPI())
			{
				case RendererAPI::API::None:    return 0;
				case RendererAPI::API::OpenGL:  return 1; // OpenGL通常使用单缓冲（即不使用多帧缓冲），因为它是一个立即模式渲染API，渲染命令直接提交到GPU执行，不需要预先准备多个帧数据。
				case RendererAPI::API::Vulkan:  return 3; // Vulkan通常使用三缓冲
			}
		};

		static constexpr uint32_t MAX_FRAMES_MODIFY()
		{
			switch (RendererAPI::GetAPI())
			{
				case RendererAPI::API::None:    return 0;
				case RendererAPI::API::OpenGL:  return 1;
				case RendererAPI::API::Vulkan:  return 0;
			}
		}
	}
}

