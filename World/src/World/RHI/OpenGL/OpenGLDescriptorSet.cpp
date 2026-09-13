#include "wldpch.h"
#include "OpenGLDescriptorSet.h"
#include "OpenGLBuffer.h"
#include "OpenGLTexture.h"
#include "OpenGLSampler.h"

namespace World::Rhi::OpenGL
{
	void OpenGLDescriptorSet::Update(const std::vector<DescriptorWrite>& writes)
	{
		m_Writes = writes;
	}

	void OpenGLDescriptorSet::Bind() const
	{
		for (const auto& write : m_Writes)
		{
			const GLuint unit = write.Binding + write.ArrayIndex;
			switch (write.Type)
			{
				case DescriptorType::UniformBuffer:
				{
					const auto buffer = std::dynamic_pointer_cast<OpenGLBuffer>(write.Buffer);
					if (buffer)
						glBindBufferRange(GL_UNIFORM_BUFFER, unit, buffer->GetID(), write.BufferOffset,
							write.BufferRange ? write.BufferRange : buffer->GetDesc().Size - write.BufferOffset);
					break;
				}
				case DescriptorType::StorageBuffer:
				case DescriptorType::InputAttachment:
				{
					const auto buffer = std::dynamic_pointer_cast<OpenGLBuffer>(write.Buffer);
					if (buffer)
						glBindBufferRange(GL_SHADER_STORAGE_BUFFER, unit, buffer->GetID(), write.BufferOffset,
							write.BufferRange ? write.BufferRange : buffer->GetDesc().Size - write.BufferOffset);
					break;
				}
				case DescriptorType::CombinedImageSampler:
				case DescriptorType::SampledImage:
				{
					const auto texture = std::dynamic_pointer_cast<OpenGLTexture>(write.Texture);
					if (texture)
						glBindTextureUnit(unit, texture->GetID());
					const auto sampler = std::dynamic_pointer_cast<OpenGLSampler>(write.Sampler);
					glBindSampler(unit, sampler ? sampler->GetID() : 0);
					break;
				}
				case DescriptorType::Sampler:
				{
					const auto sampler = std::dynamic_pointer_cast<OpenGLSampler>(write.Sampler);
					if (sampler)
						glBindSampler(unit, sampler->GetID());
					break;
				}
				case DescriptorType::StorageImage:
				{
					const auto texture = std::dynamic_pointer_cast<OpenGLTexture>(write.Texture);
					if (texture)
						glBindImageTexture(unit, texture->GetID(), write.BaseMipLevel, GL_TRUE,
							write.BaseArrayLayer, GL_READ_WRITE, GL_RGBA8);
					break;
				}
			}
		}
	}
}
