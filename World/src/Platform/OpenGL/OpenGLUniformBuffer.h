#pragma once

#include "World/Renderer/UniformBuffer.h"

namespace World
{
	class OpenGLUniformBuffer : public UniformBuffer
	{
	public:
		OpenGLUniformBuffer(uint32_t size, uint32_t binding);
		virtual ~OpenGLUniformBuffer();

		virtual void SetData(const void* data, uint32_t size, uint32_t offset = 0) override;
		virtual uint32_t GetBinding() const override { return m_Binding; }
		virtual void Bind(uint32_t binding) override;
	private:
		uint32_t m_RendererID = 0;
		uint32_t m_Binding = 0;
	};
}