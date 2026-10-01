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
		// HOTR-P2C:上传语义提示。true = 后端用**同步**上传路径(提交后等待),不并入异步上传环。
		// 材质贴图(尤其热重载/产物切换)会在活动帧中途被替换,实测异步环路径会打坏帧同步
		// (VUID 01123/01779 → 00045/00071 → device lost);这类纹理加载低频,同步代价可接受。
		// GL 后端忽略该字段(它本来就是同步上传)。
		bool SynchronousUpload = false;
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

	// HOTR-P2C-ROOT:一次调用上传的多 mip 数据切片(块压缩产物的 mip 链在容器里逐块存储,
	// 无法用一段连续数据描述,所以逐条给)。
	struct TextureMipUpload
	{
		uint32_t Mip = 0;
		const void* Data = nullptr;
		uint64_t Size = 0;
	};

	class WLD_API Texture
	{
	public:
		virtual ~Texture() = default;
		virtual const TextureDesc& GetDesc() const = 0;
		// 上传完整图像数据(data 按 layer-major,mip 次之);offset 为 mip 0 起点。
		virtual void SetData(const void* data, uint64_t size, uint32_t layer = 0, uint32_t mip = 0) = 0;
		// 一次上传多条 mip。后端契约:这一批在**同一个提交**里完成
		// (Vulkan = 异步上传环一次 Submit;GL = 逐条同步,语义等价)。
		// 默认实现 = 逐条 SetData(兼容旧后端);Vulkan/Vulkan 后端覆盖为批量提交 —— 见
		// docs/dev/hot-reload.md 与 tools/agents/tasks 的 P2-c 取证记录(逐 mip 多次异步提交
		// 会把帧同步状态打坏)。
		virtual void SetDataMips(const TextureMipUpload* mips, std::size_t count)
		{
			if (!mips)
				return;
			for (std::size_t index = 0; index < count; ++index)
				if (mips[index].Data && mips[index].Size > 0)
					SetData(mips[index].Data, mips[index].Size, 0, mips[index].Mip);
		}
	};
}
