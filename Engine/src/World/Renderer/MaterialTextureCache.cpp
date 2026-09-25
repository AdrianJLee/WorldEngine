#include "wldpch.h"

#include "World/Renderer/MaterialTextureCache.h"

#include "World/Core/Log.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/TextureData.h"

#include <vector>

namespace World
{
	namespace
	{
		// 产物块格式 + 产物头里的 sRGB 标志 → RHI 格式(契约:运行时格式全部来自产物头,
		// 不再由材质槽位猜;槽位只决定"要不要产物"这件事之外的兜底路径)。
		Rhi::Format ArtifactRhiFormat(TextureBlockFormat block, bool srgb)
		{
			switch (block)
			{
				case TextureBlockFormat::Rgba8:
					return srgb ? Rhi::Format::R8G8B8A8_SRGB : Rhi::Format::R8G8B8A8_UNORM;
				case TextureBlockFormat::Rgba16f:
					return Rhi::Format::R16G16B16A16_SFLOAT;
				case TextureBlockFormat::Bc1:
					return srgb ? Rhi::Format::BC1_UNORM_SRGB : Rhi::Format::BC1_UNORM;
				case TextureBlockFormat::Bc3:
					return srgb ? Rhi::Format::BC3_UNORM_SRGB : Rhi::Format::BC3_UNORM;
				case TextureBlockFormat::Bc7:
					return srgb ? Rhi::Format::BC7_UNORM_SRGB : Rhi::Format::BC7_UNORM;
				case TextureBlockFormat::Bc5:
					return Rhi::Format::BC5_UNORM;
				case TextureBlockFormat::Bc4:
					return Rhi::Format::BC4_UNORM;
				default:
					return Rhi::Format::Undefined;
			}
		}

		// 按产物头建纹理 + 逐 mip 上传。任何一步失败都返回 nullptr 并记一条警告,
		// 由调用方回退源图路径(不崩、不黑、不静默)。
		Rhi::Handle<Rhi::Texture> CreateTextureFromArtifact(const std::string& path, const TextureAsset& asset)
		{
			const TextureArtifactHeader& header = asset.Header;
			Rhi::Device* device = Renderer::GetDevice().get();
			const Rhi::Format format = ArtifactRhiFormat(header.Format, header.Srgb);
			if (format == Rhi::Format::Undefined)
			{
				WLD_CORE_WARN("[material] texture '{0}': artifact format {1} has no RHI mapping; "
					"falling back to the source image", path, TextureBlockFormatName(header.Format));
				return nullptr;
			}
			if (IsBlockCompressed(header.Format) && !device->GetCapabilities().TextureCompressionBC)
			{
				WLD_CORE_WARN("[material] texture '{0}': device does not support BC texture compression; "
					"falling back to the source image", path);
				return nullptr;
			}

			// 头里的 mipCount 超过该尺寸的完整链长度 = 坏产物(解析层只看单级尺寸自洽):
			// 按链长 clamp,避免拿非法 mip 数去建纹理(Vulkan 会直接 VUID/失败)。
			const uint32_t maxMips = TextureArtifactHeader::MipCountFor(header.Width, header.Height);
			uint32_t mipLevels = header.MipCount;
			if (mipLevels > maxMips)
			{
				WLD_CORE_WARN("[material] texture '{0}': artifact declares {1} mips for {2}x{3}, "
					"clamping to {4}", path, mipLevels, header.Width, header.Height, maxMips);
				mipLevels = maxMips;
			}

			Rhi::TextureDesc desc;
			desc.Type = Rhi::TextureType::Texture2D;
			desc.Format = format;
			desc.Extent = { header.Width, header.Height, 1 };
			desc.MipLevels = mipLevels;
			desc.Usage = Rhi::TextureUsageSampled | Rhi::TextureUsageTransferDst;
			desc.DebugName = "Material." + path;
			Rhi::Handle<Rhi::Texture> texture = device->CreateTexture(desc);
			if (!texture)
			{
				WLD_CORE_WARN("[material] texture '{0}': creating a {1} texture failed; "
					"falling back to the source image", path, TextureBlockFormatName(header.Format));
				return nullptr;
			}

			for (uint32_t mip = 0; mip < mipLevels; ++mip)
			{
				const uint8_t* data = asset.MipData(mip);
				const uint64_t size = asset.MipSize(mip);
				if (!data || size == 0)
				{
					WLD_CORE_WARN("[material] texture '{0}': artifact mip {1} is missing; "
						"falling back to the source image", path, mip);
					return nullptr;
				}
				texture->SetData(data, size, /*layer*/ 0, mip);
			}

			// 产物命中的确凿证据(每次加载只打一条:缓存命中后不再走到这里)。
			WLD_CORE_INFO("[material] texture '{0}': artifact format={1} {2}x{3} mips={4} srgb={5}",
				path, TextureBlockFormatName(header.Format), header.Width, header.Height,
				mipLevels, header.Srgb ? 1 : 0);
			return texture;
		}
	}

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

	void MaterialTextureCache::Invalidate(const std::string& path)
	{
		// 空路径 = 共享白纹理,没有磁盘来源,拒绝失效(避免误清兜底资源)。
		const std::string normalized = path.empty() ? std::string() : MaterialLibrary::NormalizePath(path);
		if (normalized.empty())
			return;

		std::vector<Rhi::Handle<Rhi::Texture>> oldHandles;
		for (const bool srgb : { true, false })
		{
			const std::string key = (srgb ? "s:" : "l:") + normalized;
			const auto cached = m_Entries.find(key);
			if (cached == m_Entries.end())
				continue;
			oldHandles.push_back(cached->second.Texture);
			m_Entries.erase(cached);
		}
		if (oldHandles.empty())
			return;   // 无设备/未命中:安全 no-op(下次 Get() 走首次加载路径)

		// 旧句柄按 SceneRenderer 同款捕获式延迟释放:GL 立即执行,Vulkan 三帧/fence 后执行;
		// 无设备时 Renderer::QueueRelease 直接执行,Handle 析构本身安全。
		Renderer::QueueRelease([oldHandles]() {});
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
		// 产物优先:命中 <path>.wtexc 就直接用产物头建纹理 + 逐 mip 上传,不碰 stb 解码。
		const TextureAsset asset = LoadTextureAsset(normalizedPath, /*flipVertically*/ false);
		if (asset.FromArtifact)
		{
			Rhi::Handle<Rhi::Texture> texture = CreateTextureFromArtifact(normalizedPath, asset);
			if (texture)
				return texture;
			// 失败原因已记日志;继续走下面的源图回退(行为与 P2 之前一致)。
		}

		const TextureData data = asset.FromArtifact
			? LoadTextureData(normalizedPath, /*flipVertically*/ false)
			: asset.Source;
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
