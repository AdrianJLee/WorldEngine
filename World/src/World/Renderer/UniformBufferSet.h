#pragma once
#include "UniformBuffer.h"

namespace World
{
	class UniformBufferSet
	{
	public:
		static Ref<UniformBufferSet> Create(uint32_t binding, int32_t size, uint32_t framesInFlight = 3);
	public:
		virtual ~UniformBufferSet() = default;

		// 获取特定帧索引对应的 UniformBuffer
		virtual Ref<UniformBuffer> Get(uint32_t frame) = 0;

		// 辅助方法：一次性设置所有帧的数据（用于不随帧改变的全局数据）
		virtual void SetData(const void* data, uint32_t size, uint32_t offset = 0) = 0;
	};
}