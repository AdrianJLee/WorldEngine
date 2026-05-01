#pragma once
#include "World/Renderer/RendererAPI.h"

#include <glm/glm.hpp>

namespace World
{
	class RenderCommand
	{
	public:
		inline static void Init()
		{
			s_RendererAPI->Init();
		}
		inline static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
		{
			s_RendererAPI->SetViewport(x, y, width, height);
		}

		inline static void DrawIndexed(const Ref<class VertexArray>& vertexArray, uint32_t indexCount = 0)
		{
			s_RendererAPI->DrawIndexed(vertexArray, indexCount);
		}

		inline static void DrawLines(const Ref<class VertexArray>& vertexArray, uint32_t vertexCount)
		{
			s_RendererAPI->DrawLines(vertexArray, vertexCount);
		}

		inline static void SetLineWidth(float width)
		{
			s_RendererAPI->SetLineWidth(width);
		}

		inline static void BeginRenderPass(const Ref<RenderPass>& renderPass, bool clear)
		{
			s_RendererAPI->BeginRenderPass(renderPass, clear);
		}

		inline static void EndRenderPass()
		{
			s_RendererAPI->EndRenderPass();
		}

		inline static uint32_t GetMaxFramesInFlight() { return s_RendererAPI->GetMaxFramesInFlight(); }
	private:
		static class RendererAPI* s_RendererAPI;
	};
}

