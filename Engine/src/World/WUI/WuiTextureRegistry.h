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

		WuiTextureRegistry();
		~WuiTextureRegistry();

		uint64_t Register(const Rhi::Handle<Rhi::Texture>& texture);
		uint64_t RegisterTexture2D(const Ref<Texture2D>& texture);
		void Update(uint64_t id, const Rhi::Handle<Rhi::Texture>& texture);
		Rhi::Handle<Rhi::Texture> Resolve(uint64_t id);
		void Clear();
		// 注册表被清空(设备重建)→ 宿主据此重注册图标/场景纹理。
		uint32_t Generation() const { return m_Generation; }
		// 内容代:任何"真的换了纹理对象"的登记/替换都会 +1(同句柄重复 Update 不变)。
		// WUI 后端的描述符集缓存按纹理对象**地址**做键,地址会被分配器复用:
		// 只按句柄指针查缓存会命中"已销毁贴图"的旧 set → 采到野视图 → 设备丢失。
		// 因此后端必须同时监听这个计数器(实测:反复拉伸视口 60 次必现 device lost)。
		uint32_t ContentRevision() const { return m_ContentRevision; }

	private:
		struct Entry
		{
			Rhi::Handle<Rhi::Texture> Texture;
			Ref<Texture2D> Source;
		};
		uint64_t m_NextId = 1;
		uint32_t m_Generation = 0;
		uint32_t m_ContentRevision = 0;
		std::unordered_map<uint64_t, Entry> m_Textures;
		std::unordered_map<const void*, uint64_t> m_TextureIds;
	};
}
