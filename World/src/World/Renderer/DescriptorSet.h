#pragma once
#include "UniformBufferSet.h"

#include <map>
namespace World
{
	struct u_TextureData
	{
		uint32_t Index;
		uint32_t Padding[3]; // 填充12个字节凑齐16字节
	};

	class DescriptorSet
	{
	public:
		void AddUniformBufferSet(uint32_t binding, uint32_t size, std::string name = "", uint32_t maxFrames = 3);

		Ref<UniformBufferSet> GetUniformBufferSet(uint32_t binding);
		Ref<UniformBufferSet> GetUniformBufferSet(std::string name);

	private:
		// <binding, UniformBufferSet>
		std::map<uint32_t, Ref<UniformBufferSet>> m_GlobalUniformBuffers;

		// <name,binding>
		std::map<std::string, uint32_t> m_BindingMap;
	};
}