#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	struct TextureDesc
	{
		TextureType Type = TextureType::Texture2D;
		Format Format = Format::R8G8B8A8_UNORM;
		Extent3D Extent {};
		uint32_t MipLevels = 1;
		uint32_t ArrayLayers = 1;
		SampleCount Samples = SampleCount::Count1;
		uint32_t Usage = TextureUsageSampled;
		std::string DebugName;
	};

	struct TextureViewDesc
	{
		Format Format = Format::Undefined;   // Undefined = 继承基纹理
		uint32_t BaseMipLevel = 0;
		uint32_t MipLevelCount = 1;
		uint32_t BaseArrayLayer = 0;
		uint32_t ArrayLayerCount = 1;
	};

	class WLD_API Texture
	{
	public:
		virtual ~Texture() = default;
		virtual const TextureDesc& GetDesc() const = 0;
		// 上传完整图像数据(data 按 layer-major,mip 次之);offset 为 mip 0 起点。
		virtual void SetData(const void* data, uint64_t size, uint32_t layer = 0, uint32_t mip = 0) = 0;
	};
}
