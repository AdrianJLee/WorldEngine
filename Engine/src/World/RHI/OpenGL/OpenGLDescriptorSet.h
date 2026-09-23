#pragma once

#include "World/RHI/RhiDescriptorSet.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	class OpenGLDescriptorSetLayout : public DescriptorSetLayout
	{
	public:
		explicit OpenGLDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) : m_Desc(desc) {}
		const DescriptorSetLayoutDesc& GetDesc() const override { return m_Desc; }

	private:
		DescriptorSetLayoutDesc m_Desc;
	};

	class OpenGLDescriptorSet : public DescriptorSet
	{
	public:
		explicit OpenGLDescriptorSet(const Handle<DescriptorSetLayout>& layout) : m_Layout(layout) {}
		void Update(const std::vector<DescriptorWrite>& writes) override;
		void Bind() const;

	private:
		Handle<DescriptorSetLayout> m_Layout;
		std::vector<DescriptorWrite> m_Writes;
	};
}
