#include "wldpch.h"
#include "DescriptorSet.h"

namespace World
{
	void DescriptorSet::AddUniformBufferSet(uint32_t binding, uint32_t size, std::string name, uint32_t maxFrames)
	{
		if (name.empty())
		{
			m_GlobalUniformBuffers[binding] = UniformBufferSet::Create(binding, size, maxFrames);
		}
		else
		{
			m_GlobalUniformBuffers[binding] = UniformBufferSet::Create(binding, size, maxFrames);
			m_BindingMap[name] = binding;
		}
	}
	Ref<UniformBufferSet> DescriptorSet::GetUniformBufferSet(uint32_t binding)
	{
		auto it = m_GlobalUniformBuffers.find(binding);
		if (it != m_GlobalUniformBuffers.end())
		{
			return it->second;
		}
		else
		{
			WLD_CORE_WARN("UniformBufferSet with binding {0} not found!", binding);
			return nullptr;
		}
	}
	Ref<UniformBufferSet> DescriptorSet::GetUniformBufferSet(std::string name)
	{
		auto it = m_BindingMap.find(name);
		if (it != m_BindingMap.end())
		{
			uint32_t binding = it->second;
			return GetUniformBufferSet(binding);
		}
		else
		{
			WLD_CORE_WARN("UniformBufferSet with name '{0}' not found!", name);
			return nullptr;
		}
	}
}