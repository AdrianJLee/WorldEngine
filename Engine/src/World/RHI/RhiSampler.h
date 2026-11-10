#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	struct SamplerDesc
	{
		Filter MinFilter = Filter::Linear;
		Filter MagFilter = Filter::Linear;
		SamplerMipmapMode MipmapMode = SamplerMipmapMode::Linear;
		SamplerAddressMode AddressU = SamplerAddressMode::Repeat;
		SamplerAddressMode AddressV = SamplerAddressMode::Repeat;
		SamplerAddressMode AddressW = SamplerAddressMode::Repeat;
		float MipLodBias = 0.0f;
		float MinLod = 0.0f;
		float MaxLod = 1000.0f;
		float MaxAnisotropy = 1.0f;
		bool EnableCompare = false;
		CompareOp Compare = CompareOp::Never;
		BorderColor Border = BorderColor::TransparentBlack;
		bool UnnormalizedCoordinates = false;
		std::string DebugName;
	};

	class WLD_API Sampler
	{
	public:
		virtual ~Sampler() = default;
		virtual const SamplerDesc& GetDesc() const = 0;
	};
}
