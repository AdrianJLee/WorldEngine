#pragma once

#include "World/Renderer/RendererAPI.h"
namespace World
{
	class OpenGLRendererAPI :public RendererAPI
	{
	public:
		virtual void Init() override;
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;

		virtual void DrawIndexed(const Ref<class VertexArray>& vertexArray, uint32_t indexCount = 0) override;
		virtual void DrawLines(const Ref<class VertexArray>& vertexArray, uint32_t vertexCount) override;
		virtual void SetLineWidth(float width) override;

		virtual void BeginRenderPass(const Ref<RenderPass>& renderPass, bool clear) override;
		virtual void EndRenderPass()override;

	private:
	};

}

