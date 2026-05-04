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
		SetTextures(binding, { texture });
	}
	void DescriptorSet::SetTextures(uint32_t binding, const std::array<Ref<Texture2D>, 32>& textures)
	{
		m_Textures[binding] = std::vector<Ref<Texture2D>>(textures.begin(), textures.end());
	}
	void DescriptorSet::Bind(uint32_t frameIndex)
	{
		// 1. 绑定所有 UBO 到各自的 binding 点
		for (auto& [binding, uboSet] : m_UniformBuffers)
		{
			if (auto ubo = uboSet->Get(frameIndex))
			{
				ubo->Bind(binding);
			}
		}

		// 2. 绑定所有纹理到各自的槽位 (对应 Renderer2D 的 32 个槽位)
		for (auto& [binding, textureList] : m_Textures)
		{
			for (uint32_t i = 0; i < textureList.size(); i++)
			{
				if (textureList[i] != nullptr)
				{
					// 计算实际的纹理槽位：binding 点 + 数组偏移
					textureList[i]->Bind(binding + i);
				}
			}
		}
	}
}