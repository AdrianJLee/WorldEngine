#include "wldpch.h"
#include "OpenGLRendererAPI.h"
#include "World/Renderer/VertexArray.h"
#include "World/Renderer/Buffer.h"

#include <glad/glad.h>
namespace World
{
	void OpenGLRendererAPI::Init()
	{
		WLD_PROFILE_FUNCTION();

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glEnable(GL_LINE_SMOOTH);
	}
	void OpenGLRendererAPI::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		glViewport(x, y, width, height);
	}

	void OpenGLRendererAPI::DrawIndexed(const Ref<class VertexArray>& vertexArray, uint32_t indexCount)
	{
		vertexArray->Bind();
		uint32_t count = indexCount ? indexCount : vertexArray->GetIndexBuffer()->GetCount();

		glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);

	}
	void OpenGLRendererAPI::DrawLines(const Ref<class VertexArray>& vertexArray, uint32_t vertexCount)
	{
		vertexArray->Bind();

		glDrawArrays(GL_LINES, 0, vertexCount);
	}
	void OpenGLRendererAPI::SetLineWidth(float width)
	{
		glLineWidth(width);
	}
	void OpenGLRendererAPI::BeginRenderPass(const Ref<RenderPass>& renderPass)
	{
		const auto& spec = renderPass->GetSpecification();
		glClearColor(spec.ClearColor.r, spec.ClearColor.g, spec.ClearColor.b, spec.ClearColor.a);

		GLbitfield flags = 0;
		if (spec.ClearOnColor) flags |= GL_COLOR_BUFFER_BIT;
		if (spec.ClearOnDepth) flags |= GL_DEPTH_BUFFER_BIT;

		glClear(flags);
	}
	void OpenGLRendererAPI::BeginRenderPass(const Ref<RenderPass>& renderPass, bool clear)
	{
		const auto& spec = renderPass->GetSpecification();

		if (spec.TargetFramebuffer)
			spec.TargetFramebuffer->Bind();
		else
		{
			WLD_CORE_WARN("RenderPass has no target framebuffer! Rendering to default framebuffer.");
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}

		if (clear)
		{
			glClearColor(spec.ClearColor.r, spec.ClearColor.g, spec.ClearColor.b, spec.ClearColor.a);

			GLbitfield flags = 0;
			if (spec.ClearOnColor) flags |= GL_COLOR_BUFFER_BIT;
			if (spec.ClearOnDepth) flags |= GL_DEPTH_BUFFER_BIT;

			glClear(flags);

			spec.TargetFramebuffer->ClearAttachment(1, -1);
		}
	}
	void OpenGLRendererAPI::EndRenderPass()
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
}