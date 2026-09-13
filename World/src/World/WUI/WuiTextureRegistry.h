#pragma once

#include "World/RHI/Rhi.h"
#include "World/Renderer/Texture.h"

#include <cstdint>
#include <unordered_map>

namespace World::Wui
{
	// 后端无关的图像注册表:面板持有的纹理 id 在 OpenGL/Vulkan 间可重建。
	class WLD_API WuiTextureRegistry
	{
	public:
		static WuiTextureRegistry& Get();

		uint64_t Register(const Rhi::Handle<Rhi::Texture>& texture);
		uint64_t RegisterTexture2D(const Ref<Texture2D>& texture);
		Rhi::Handle<Rhi::Texture> Resolve(uint64_t id);
		void Clear();
		uint32_t Generation() const { return m_Generation; }

	private:
		struct Entry
		{
			Rhi::Handle<Rhi::Texture> Texture;
			Ref<Texture2D> Source;
		};
		uint64_t m_NextId = 1;
		uint32_t m_Generation = 0;
		std::unordered_map<uint64_t, Entry> m_Textures;
	};
}
