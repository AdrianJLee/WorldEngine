#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	class Sampler;

	struct DescriptorBinding
	{
		uint32_t Binding = 0;
		DescriptorType Type = DescriptorType::CombinedImageSampler;
		ShaderStageFlags Stages = 0;
		uint32_t Count = 1;
		bool VariableCount = false;   // bindless:运行时数组大小
	};

	struct DescriptorSetLayoutDesc
	{
		std::vector<DescriptorBinding> Bindings;   // 按 Binding 排序,不许重复
		std::string DebugName;
	};

	struct DescriptorWrite
	{
		uint32_t Binding = 0;
		uint32_t ArrayIndex = 0;
		DescriptorType Type = DescriptorType::CombinedImageSampler;
		Handle<Buffer> Buffer;             // Uniform/Storage
		uint64_t BufferOffset = 0;
		uint64_t BufferRange = 0;          // 0 = 全尺寸
		Handle<Rhi::Texture> Texture;      // Sampled/Storage/Input
		Handle<Sampler> Sampler;           // 独立 Sampler(可选)
		uint32_t BaseMipLevel = 0;
		uint32_t MipLevelCount = 1;
		uint32_t BaseArrayLayer = 0;
		uint32_t ArrayLayerCount = 1;
	};

	class WLD_API DescriptorSetLayout
	{
	public:
		virtual ~DescriptorSetLayout() = default;
		virtual const DescriptorSetLayoutDesc& GetDesc() const = 0;
	};

	class WLD_API DescriptorSet
	{
	public:
		virtual ~DescriptorSet() = default;
		virtual void Update(const std::vector<DescriptorWrite>& writes) = 0;
	};
}
