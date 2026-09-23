#pragma once
#include "World/Renderer/VertexArray.h"
#include "World/Renderer/Shader.h"

namespace World
{
	class OpenGLVertexArray :public VertexArray
	{
	public:
		OpenGLVertexArray();
		virtual ~OpenGLVertexArray();
		virtual void Bind() const override;
		virtual void Unbind() const override;
		virtual void AddVertexBuffer(const Ref<class VertexBuffer>& vertexBuffer, const Ref<Shader>& shader) override;
		virtual void SetIndexBuffer(const Ref<class IndexBuffer>& indexBuffer) override;
		virtual const std::vector<Ref<class VertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
		virtual const Ref<class IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }
	private:
		uint32_t m_RendererID;
		std::vector<Ref<class VertexBuffer>> m_VertexBuffers;
		Ref<class IndexBuffer> m_IndexBuffer;
	};
}
