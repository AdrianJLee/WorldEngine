#include "wldpch.h"

#include "World/Renderer/MaterialTextureCache.h"

#include "World/Core/Log.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/TextureData.h"

namespace World
{
	MaterialTextureCache& MaterialTextureCache::Get()
	{
		static MaterialTextureCache* instance = new MaterialTextureCache();
		return *instance;
	}

	void MaterialTextureCache::Shutdown()
	{
		Get().Clear();
	}

	void MaterialTextureCache::Clear()
	{
		m_Entries.clear();
		m_Device = nullptr;
	}

	Rhi::Handle<Rhi::Texture> MaterialTextureCache::CreateWhite()
	{
		Rhi::TextureDesc desc;
		desc.Type = Rhi::TextureType::Texture2D;
		desc.Format = Rhi::Format::R8G8B8A8_UNORM;
		desc.Extent = { 1, 1, 1 };
		desc.Usage = Rhi::TextureUsageSampled | Rhi::TextureUsageTransferDst;
		desc.DebugName = "Material.White";
		Rhi::Handle<Rhi::Texture> texture = Renderer::GetDevice()->CreateTexture(desc);
		if (texture)
		{
			static const uint8_t white[4] = { 255, 255, 255, 255 };
			texture->SetData(white, sizeof(white));
		}
		return texture;
	}

	Rhi::Handle<Rhi::Texture> MaterialTextureCache::Load(const std::string& normalizedPath, bool srgb)
	{
		const TextureData data = LoadTextureData(normalizedPath, /*flipVertically*/ false);
		if (std::getenv("WLD_TRACE_3D"))
			WLD_CORE_INFO("[material] load texture '{0}' srgb={1} valid={2} size={3}x{4}",
				normalizedPath, static_cast<int>(srgb), static_cast<int>(data.Valid), data.Width, data.Height);
		Rhi::TextureDesc desc;
		desc.Type = Rhi::TextureType::Texture2D;
		// albedo/emissive 类贴图以 sRGB 格式创建:采样时硬件解码到线性(与 2D/WUI 无关)。
		desc.Format = srgb ? Rhi::Format::R8G8B8A8_SRGB : Rhi::Format::R8G8B8A8_UNORM;
		desc.Extent = { data.Width, data.Height, 1 };
		desc.Usage = Rhi::TextureUsageSampled | Rhi::TextureUsageTransferDst;
		desc.DebugName = "Material." + normalizedPath;
		Rhi::Handle<Rhi::Texture> texture = Renderer::GetDevice()->CreateTexture(desc);
		if (texture && !data.Pixels.empty())
			texture->SetData(data.Pixels.data(), data.Pixels.size());
		return texture;
	}

	Rhi::Handle<Rhi::Texture> MaterialTextureCache::Get(const std::string& path, bool srgb)
	{
		if (!Renderer::GetDevice())
			return nullptr;

		// 设备变化(后端切换/重建)后旧句柄全部失效:整体丢弃,按新设备重建。
		if (m_Device != Renderer::GetDevice().get())
		{
			m_Entries.clear();
			m_Device = Renderer::GetDevice().get();
		}

		const std::string normalized = path.empty() ? std::string() : MaterialLibrary::NormalizePath(path);
		const std::string key = (srgb ? "s:" : "l:") + normalized;
		const auto cached = m_Entries.find(key);
		if (cached != m_Entries.end())
			return cached->second.Texture;

		Rhi::Handle<Rhi::Texture> texture;
		if (normalized.empty())
		{
			texture = CreateWhite();
		}
		else
		{
			texture = Load(normalized, srgb);
			if (!texture)
				texture = CreateWhite();
		}
		m_Entries.emplace(key, Entry { texture, texture != nullptr });
		return texture;
	}
}
