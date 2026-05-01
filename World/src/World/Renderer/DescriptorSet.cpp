#include "wldpch.h"
#include "DescriptorSet.h"

namespace World
{
	void DescriptorSet::AddUniformBufferSet(uint32_t binding, uint32_t size, uint32_t maxFrames)
	{
		m_UniformBuffers[binding] = UniformBufferSet::Create(binding, size, maxFrames);
	}
	Ref<UniformBufferSet> DescriptorSet::GetUniformBufferSet(uint32_t binding)
	{
		auto it = m_UniformBuffers.find(binding);
		if (it != m_UniformBuffers.end())
		{
			return it->second;
		}
		else
		{
			WLD_CORE_WARN("UniformBufferSet with binding {0} not found!", binding);
			return nullptr;
		}
	}

	void DescriptorSet::SetTexture(uint32_t binding, Ref<Texture2D> texture)
	{
		m_Textures[binding] = texture;
	}

	void DescriptorSet::Bind(uint32_t frameIndex)
	{
		// 1. 绑定所有 UBO 到各自的 binding 点
		for (auto& [binding, uboSet] : m_UniformBuffers)
		{
			uboSet->Get(frameIndex)->Bind(binding);
		}

		// 2. 绑定所有纹理到各自的槽位 (对应 Renderer2D 的 32 个槽位)
		for (auto& [binding, texture] : m_Textures)
		{
			if (texture)
				texture->Bind(binding);
		}
	}
}