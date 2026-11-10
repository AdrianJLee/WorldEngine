// RHI 资源字节估算(GPU 驻留统计用)。
//
// 为什么单独一个文件:VRAM 数字只有在**口径写清楚**时才有意义。这里只回答一个
// 问题——"按 desc 分配,这张纹理/这个缓冲占用多少**设备内存**",不回答"驱动实际
// 分配了多少"(后者需要 VK_EXT_memory_budget / GL 扩展,本期不引入)。
//
// 口径与边界(读数字前必看):
//   * 按 mip 逐级求和(每级 max(1, dim>>i)),与 Vulkan/GL 的分配语义一致;
//   * 未知格式返回 0 并计入 `UnaccountedTextures()`,**不静默当成 0 字节**——
//     "没算进去"和"真的 0 字节"是两件事(见 docs/dev/profiling.md 的缺失语义);
//   * 不含驱动对齐/填充、不含交换链图像(不归引擎所有)。

#include "wldpch.h"
#include "World/RHI/RhiTexture.h"

#include <atomic>

namespace World::Rhi
{
	namespace
	{
		std::atomic<uint64_t> g_UnaccountedTextures { 0 };
		std::atomic<uint64_t> g_AccountedTextures { 0 };

		// 每像素位数(未压缩格式)。0 = 未知/压缩(压缩走块计算)。
		uint32_t BitsPerPixel(Format format)
		{
			switch (format)
			{
				case Format::R8_UNORM:
				case Format::R8_SNORM:
				case Format::R8_UINT:
				case Format::R8_SINT: return 8;

				case Format::R8G8_UNORM:
				case Format::R8G8_SNORM:
				case Format::R8G8_UINT:
				case Format::R8G8_SINT:
				case Format::R16_UNORM:
				case Format::R16_SNORM:
				case Format::R16_UINT:
				case Format::R16_SINT:
				case Format::R16_SFLOAT:
				case Format::D16_UNORM: return 16;

				case Format::R8G8B8A8_UNORM:
				case Format::B8G8R8A8_UNORM:
				case Format::R8G8B8A8_SRGB:
				case Format::B8G8R8A8_SRGB:
				case Format::R8G8B8A8_SNORM:
				case Format::R8G8B8A8_UINT:
				case Format::R8G8B8A8_SINT:
				case Format::R16G16_UNORM:
				case Format::R16G16_SNORM:
				case Format::R16G16_UINT:
				case Format::R16G16_SINT:
				case Format::R16G16_SFLOAT:
				case Format::R32_UINT:
				case Format::R32_SINT:
				case Format::R32_SFLOAT:
				case Format::R10G10B10A2_UNORM:
				case Format::R11G11B10_SFLOAT:
				case Format::D24_UNORM_S8_UINT:
				case Format::D32_SFLOAT: return 32;

				case Format::R16G16B16A16_UNORM:
				case Format::R16G16B16A16_SNORM:
				case Format::R16G16B16A16_UINT:
				case Format::R16G16B16A16_SINT:
				case Format::R16G16B16A16_SFLOAT:
				case Format::R32G32_UINT:
				case Format::R32G32_SINT:
				case Format::R32G32_SFLOAT:
				case Format::D32_SFLOAT_S8_UINT: return 64;

				case Format::R32G32B32A32_UINT:
				case Format::R32G32B32A32_SINT:
				case Format::R32G32B32A32_SFLOAT: return 128;

				// 24 位 RGB 在 RHI 里没有对应枚举(R32G32B32 未定义)⇒ 落到未知分支。
				default: return 0;   // 未知或压缩
			}
		}

		// 每块字节数(BC 压缩,块 = 4×4 像素)。
		uint32_t BlockBytes(Format format)
		{
			switch (format)
			{
				case Format::BC1_UNORM:
				case Format::BC1_UNORM_SRGB:
				case Format::BC4_UNORM: return 8;
				case Format::BC2_UNORM:
				case Format::BC3_UNORM:
				case Format::BC3_UNORM_SRGB:
				case Format::BC5_UNORM:
				case Format::BC7_UNORM:
				case Format::BC7_UNORM_SRGB: return 16;
				default: return 0;
			}
		}

		uint64_t MipSize(uint64_t width, uint64_t height, uint32_t mipIndex, Format format)
		{
			const uint64_t w = (width >> mipIndex) > 1 ? (width >> mipIndex) : 1;
			const uint64_t h = (height >> mipIndex) > 1 ? (height >> mipIndex) : 1;

			if (const uint32_t bpp = BitsPerPixel(format))
				return (w * h * bpp + 7) / 8;

			if (const uint32_t block = BlockBytes(format))
			{
				const uint64_t blocksX = (w + 3) / 4;
				const uint64_t blocksY = (h + 3) / 4;
				return blocksX * blocksY * block;
			}
			return 0;
		}
	}

	uint64_t EstimateTextureBytes(const TextureDesc& desc)
	{
		const uint64_t width = desc.Extent.Width;
		const uint64_t height = desc.Extent.Height > 0 ? desc.Extent.Height : 1;
		const uint64_t depth = desc.Extent.Depth > 0 ? desc.Extent.Depth : 1;
		if (width == 0 || height == 0)
			return 0;

		const uint32_t mipLevels = desc.MipLevels > 0 ? desc.MipLevels : 1;
		uint64_t total = 0;
		uint64_t mip0 = 0;
		for (uint32_t mip = 0; mip < mipLevels; ++mip)
		{
			const uint64_t size = MipSize(width, height, mip, desc.Format);
			if (mip == 0)
				mip0 = size;
			total += size;
		}
		total *= depth;
		total *= (desc.ArrayLayers > 0 ? desc.ArrayLayers : 1);
		// 6 面立方图在枚举里没有单独类型;按 ArrayLayers 计,不重复乘。

		if (mip0 == 0)
		{
			g_UnaccountedTextures.fetch_add(1, std::memory_order_relaxed);
			return 0;
		}
		g_AccountedTextures.fetch_add(1, std::memory_order_relaxed);
		return total;
	}

	uint64_t UnaccountedTextureCount()
	{
		return g_UnaccountedTextures.load(std::memory_order_relaxed);
	}

	uint64_t AccountedTextureCount()
	{
		return g_AccountedTextures.load(std::memory_order_relaxed);
	}
}
