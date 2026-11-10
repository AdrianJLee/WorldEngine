#pragma once
#include <memory>

namespace World
{
	class VertexArray
	{
	public:
		static Ref<VertexArray> Create();
	public:
		virtual ~VertexArray() = default;
		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;
		virtual void AddVertexBuffer(const Ref<class VertexBuffer>& vertexBuffer, const Ref<class Shader>& shader) = 0;
		virtual void SetIndexBuffer(const Ref<class IndexBuffer>& indexBuffer) = 0;
		virtual const std::vector<Ref<class VertexBuffer>>& GetVertexBuffers() const = 0;
		virtual const Ref<class IndexBuffer>& GetIndexBuffer() const = 0;
	};

}

