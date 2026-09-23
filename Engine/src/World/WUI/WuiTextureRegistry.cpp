#include "wldpch.h"
#include "WuiTextureRegistry.h"

#include "World/Renderer/Renderer.h"
#include "World/RHI/RhiTextureBridge.h"

namespace World::Wui
{
	WuiTextureRegistry& WuiTextureRegistry::Get()
	{
		static WuiTextureRegistry registry;
		return registry;
	}

	WuiTextureRegistry::WuiTextureRegistry()
	{
		// 设备切换/销毁时先丢掉旧句柄,面板持有的 id 在切回后重新注册。
		Renderer::RegisterDeviceReleaseHook(this, [this] { Clear(); });
	}

	WuiTextureRegistry::~WuiTextureRegistry()
	{
		Renderer::UnregisterDeviceReleaseHook(this);
	}

	uint64_t WuiTextureRegistry::Register(const Rhi::Handle<Rhi::Texture>& texture)
	{
		if (!texture)
			return 0;
		const auto existing = m_TextureIds.find(texture.get());
		if (existing != m_TextureIds.end())
			return existing->second;
		const uint64_t id = m_NextId++;
		m_Textures[id] = { texture, nullptr };
		m_TextureIds[texture.get()] = id;
		++m_ContentRevision;
		return id;
	}

	uint64_t WuiTextureRegistry::RegisterTexture2D(const Ref<Texture2D>& texture)
	{
		if (!texture)
			return 0;
		const uint64_t id = m_NextId++;
		m_Textures[id] = { nullptr, texture };
		++m_ContentRevision;
		return id;
	}

	void WuiTextureRegistry::Update(uint64_t id, const Rhi::Handle<Rhi::Texture>& texture)
	{
		if (id == 0)
			return;
		const auto it = m_Textures.find(id);
		if (it == m_Textures.end())
			return;
		if (texture && it->second.Texture.get() == texture.get())
			return;   // 同一对象重复 Update:内容没变,不制造每帧失效
		if (it->second.Texture)
			m_TextureIds.erase(it->second.Texture.get());
		it->second.Texture = texture;
		it->second.Source = nullptr;
		if (texture)
			m_TextureIds[texture.get()] = id;
		++m_ContentRevision;
	}

	Rhi::Handle<Rhi::Texture> WuiTextureRegistry::Resolve(uint64_t id)
	{
		if (id == 0)
			return nullptr;
		const auto it = m_Textures.find(id);
		if (it == m_Textures.end())
			return nullptr;
		if (it->second.Texture)
			return it->second.Texture;
		if (it->second.Source && Renderer::GetDevice())
		{
			it->second.Texture = Rhi::WrapTexture2D(Renderer::GetDevice(), it->second.Source);
			return it->second.Texture;
		}
		return nullptr;
	}

	void WuiTextureRegistry::Clear()
	{
		m_Textures.clear();
		m_TextureIds.clear();
		++m_Generation;
		++m_ContentRevision;
	}
}
