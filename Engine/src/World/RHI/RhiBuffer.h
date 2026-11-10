#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	struct BufferDesc
	{
		uint64_t Size = 0;
		uint32_t Usage = BufferUsageNone;
		MemoryHint Memory = MemoryHint::DeviceLocal;
		std::string DebugName;
		const void* InitialData = nullptr;   // 提供时在创建期拷贝
	};

	class WLD_API Buffer
	{
	public:
		virtual ~Buffer() = default;
		virtual const BufferDesc& GetDesc() const = 0;
		// 更新 [offset, offset+dataSize);HostVisible 内存可直接 Map/Unmap。
		virtual void* Map(uint64_t offset = 0, uint64_t size = 0) = 0;
		virtual void Unmap() = 0;
		virtual void SetData(const void* data, uint64_t size, uint64_t offset = 0) = 0;
		virtual uint64_t GetGpuAddress() const { return 0; }   // 需要 bindless 时实现
	};
}
