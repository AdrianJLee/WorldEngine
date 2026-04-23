#pragma once
#include "World/Core/Core.h"

#include <unordered_map>

namespace World
{
	class UniformBuffer
	{
	public:
		virtual ~UniformBuffer() = default;

		virtual void SetData(const void* data, uint32_t size, uint32_t offset = 0) = 0;
		virtual uint32_t GetBinding() const = 0;

		static Ref<UniformBuffer> Create(uint32_t size, uint32_t binding);
	};

	struct UniformBufferResource
	{
		// <binding, UniformBuffer>
		std::unordered_map <uint32_t, Ref<UniformBuffer>> Buffers;

		Ref<UniformBuffer> GetBuffer(uint32_t binding)
		{
			if (Buffers.find(binding) != Buffers.end())
			{
				return Buffers[binding];
			}
			return nullptr;
		}

		void SetData(uint32_t binding, const void* data, uint32_t size, uint32_t offset = 0)
		{
			if (Buffers.find(binding) != Buffers.end())
			{
				Buffers[binding]->SetData(data, size, offset);
			}
			else
			{
				Buffers.insert({ binding, UniformBuffer::Create(size, binding) });
				Buffers[binding]->SetData(data, size, offset);
			}
		}
	};
}