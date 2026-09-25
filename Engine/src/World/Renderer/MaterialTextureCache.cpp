#include "wldpch.h"

#include "World/Renderer/MaterialTextureCache.h"

#include "World/Core/Log.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/TextureData.h"
#include "World/Renderer/TextureImportSettings.h"

#include <algorithm>
#include <string>
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

		const char* WrapName(TextureWrap wrap)
		{
			switch (wrap)
			{
				case TextureWrap::Repeat: return "repeat";
				case TextureWrap::Clamp: return "clamp";
				case TextureWrap::Mirror: return "mirror";
			}
			return "?";
		}

		const char* FilterName(TextureFilter filter)
		{
			switch (filter)
			{
				case TextureFilter::Point: return "point";
				case TextureFilter::Bilinear: return "bilinear";
				case TextureFilter::Trilinear: return "trilinear";
			}
			return "?";
		}

		// 产物头的采样状态 → RHI 采样器描述。未知枚举值只警告并按安全默认(repeat / trilinear)走;
		// 各向异性按设备能力 clamp(与 Renderer3D 共享 sampler 同一口径,Vulkan 不会撞
		// VUID-VkSamplerCreateInfo-anisotropyEnable-01071)。
		Rhi::SamplerDesc ArtifactSamplerDesc(const std::string& path, const TextureArtifactHeader& header,
			uint32_t& outRequestedAnisotropy)
		{
			Rhi::SamplerDesc desc;
			switch (static_cast<TextureWrap>(header.Wrap))
			{
				case TextureWrap::Repeat:
					desc.AddressU = desc.AddressV = desc.AddressW = Rhi::SamplerAddressMode::Repeat;
					break;
				case TextureWrap::Clamp:
					desc.AddressU = desc.AddressV = desc.AddressW = Rhi::SamplerAddressMode::ClampToEdge;
					break;
				case TextureWrap::Mirror:
					desc.AddressU = desc.AddressV = desc.AddressW = Rhi::SamplerAddressMode::MirroredRepeat;
					break;
				default:
					WLD_CORE_WARN("[material] texture '{0}': artifact wrap={1} is unknown; using repeat",
						path, header.Wrap);
					desc.AddressU = desc.AddressV = desc.AddressW = Rhi::SamplerAddressMode::Repeat;
					break;
			}
			switch (static_cast<TextureFilter>(header.Filter))
			{
				case TextureFilter::Point:
					desc.MinFilter = Rhi::Filter::Nearest;
					desc.MagFilter = Rhi::Filter::Nearest;
					desc.MipmapMode = Rhi::SamplerMipmapMode::Nearest;
					break;
				case TextureFilter::Bilinear:
					desc.MinFilter = Rhi::Filter::Linear;
					desc.MagFilter = Rhi::Filter::Linear;
					desc.MipmapMode = Rhi::SamplerMipmapMode::Nearest;
					break;
				case TextureFilter::Trilinear:
					desc.MinFilter = Rhi::Filter::Linear;
					desc.MagFilter = Rhi::Filter::Linear;
					desc.MipmapMode = Rhi::SamplerMipmapMode::Linear;
					break;
				default:
					WLD_CORE_WARN("[material] texture '{0}': artifact filter={1} is unknown; using trilinear",
						path, header.Filter);
					desc.MinFilter = Rhi::Filter::Linear;
					desc.MagFilter = Rhi::Filter::Linear;
					desc.MipmapMode = Rhi::SamplerMipmapMode::Linear;
					break;
			}

			outRequestedAnisotropy = std::clamp(header.Anisotropy, 1u, 16u);
			float anisotropy = static_cast<float>(outRequestedAnisotropy);
			const Rhi::Capabilities& capabilities = Renderer::GetDevice()->GetCapabilities();
			if (anisotropy > 1.0f && !capabilities.AnisotropicFiltering)
				anisotropy = 1.0f;
			else if (anisotropy > capabilities.MaxSamplerAnisotropy)
				anisotropy = std::max(1.0f, capabilities.MaxSamplerAnisotropy);
			desc.MaxAnisotropy = anisotropy;
			return desc;
		}

		// 按产物头建纹理 + 逐 mip 上传。任何一步失败都返回 nullptr 并记一条警告,
		// 由调用方回退源图路径(不崩、不黑、不静默)。
		// 产物元信息经 ArtifactLoadInfo 回传,由 Load 拷进缓存条目(避免匿名命名空间碰私有嵌套类型)。
		struct ArtifactLoadInfo
		{
			TextureBlockFormat Format = TextureBlockFormat::Rgba8;
			TextureWrap Wrap = TextureWrap::Repeat;
			TextureFilter Filter = TextureFilter::Trilinear;
			uint32_t RequestedAnisotropy = 1;
			Rhi::SamplerDesc Sampler;
		};

		Rhi::Handle<Rhi::Texture> CreateTextureFromArtifact(const std::string& path, const TextureAsset& asset,
			ArtifactLoadInfo& out)
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

			// M4-TEX P2b:把产物元信息挂到缓存条目上,供 IsBc5Artifact / GetSampler 查询。
			out.Format = header.Format;
			out.Wrap = static_cast<TextureWrap>(header.Wrap);
			out.Filter = static_cast<TextureFilter>(header.Filter);
			out.Sampler = ArtifactSamplerDesc(path, header, out.RequestedAnisotropy);
			out.Sampler.DebugName = "Material." + path + ".Sampler";
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
		m_Samplers.clear();
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

	Rhi::Handle<Rhi::Texture> MaterialTextureCache::Load(const std::string& normalizedPath, bool srgb, Entry& out)
	{
		// 产物优先:命中 <path>.wtexc 就直接用产物头建纹理 + 逐 mip 上传,不碰 stb 解码。
		const TextureAsset asset = LoadTextureAsset(normalizedPath, /*flipVertically*/ false);
		if (asset.FromArtifact)
		{
			ArtifactLoadInfo info;
			Rhi::Handle<Rhi::Texture> texture = CreateTextureFromArtifact(normalizedPath, asset, info);
			if (texture)
			{
				out.FromArtifact = true;
				out.Format = info.Format;
				out.Wrap = info.Wrap;
				out.Filter = info.Filter;
				out.RequestedAnisotropy = info.RequestedAnisotropy;
				out.Sampler = info.Sampler;
				return texture;
			}
			// 失败原因已记日志;继续走下面的源图回退(行为与 P2 之前一致)。
		}

		// 产物在但建纹理失败 / 没产物:回退解码**源图**。资产引用必须先解析到源图 ——
		// 直接拿 `.wtex` 去 stb 只会拿到 1x1 白兜底(实测:'AssetOnly.wtex' → 白球)。
		const TextureData data = asset.FromArtifact
			? LoadTextureData(ResolveTextureSourcePath(normalizedPath), /*flipVertically*/ false)
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

	bool MaterialTextureCache::IsBc5Artifact(const std::string& path, bool srgb) const
	{
		if (!Renderer::GetDevice() || m_Device != Renderer::GetDevice().get() || path.empty())
			return false;
		const std::string normalized = MaterialLibrary::NormalizePath(path);
		const auto cached = m_Entries.find((srgb ? "s:" : "l:") + normalized);
		return cached != m_Entries.end() && cached->second.FromArtifact
			&& cached->second.Format == TextureBlockFormat::Bc5;
	}

	Rhi::Handle<Rhi::Sampler> MaterialTextureCache::GetSampler(const std::string& path, bool srgb)
	{
		Rhi::Device* device = Renderer::GetDevice().get();
		if (!device)
			return nullptr;

		// 设备变化(后端切换/重建)后旧句柄全部失效:纹理与采样器一起丢弃(防地址/句柄复用串味)。
		if (m_Device != device)
		{
			m_Entries.clear();
			m_Samplers.clear();
			m_Device = device;
		}

		const std::string normalized = path.empty() ? std::string() : MaterialLibrary::NormalizePath(path);
		if (normalized.empty())
			return nullptr;   // 共享白纹理没有产物来源,继续用共享 sampler
		const auto cached = m_Entries.find((srgb ? "s:" : "l:") + normalized);
		if (cached == m_Entries.end() || !cached->second.FromArtifact)
			return nullptr;   // 回退(stb)路径:调用方继续用共享 sampler,行为不变

		const Entry& entry = cached->second;
		const Rhi::SamplerDesc& desc = entry.Sampler;
		const std::string key = std::to_string(static_cast<int>(desc.AddressU)) + ":"
			+ std::to_string(static_cast<int>(desc.MinFilter)) + ":"
			+ std::to_string(static_cast<int>(desc.MipmapMode)) + ":"
			+ std::to_string(static_cast<int>(desc.MaxAnisotropy));
		const auto found = m_Samplers.find(key);
		if (found != m_Samplers.end())
			return found->second;

		Rhi::Handle<Rhi::Sampler> sampler = device->CreateSampler(desc);
		if (!sampler)
		{
			WLD_CORE_WARN("[material] texture '{0}': creating the artifact sampler failed; "
				"using the shared sampler", normalized);
			return nullptr;
		}
		m_Samplers.emplace(key, sampler);
		const std::string clampNote = desc.MaxAnisotropy < static_cast<float>(entry.RequestedAnisotropy)
			? " (clamped from " + std::to_string(entry.RequestedAnisotropy) + ")" : std::string();
		WLD_CORE_INFO("[material] per-texture sampler '{0}': wrap={1} filter={2} aniso={3}{4}",
			normalized, WrapName(entry.Wrap), FilterName(entry.Filter),
			static_cast<int>(desc.MaxAnisotropy), clampNote);
		return sampler;
	}

	Rhi::Handle<Rhi::Texture> MaterialTextureCache::Get(const std::string& path, bool srgb)
	{
		if (!Renderer::GetDevice())
			return nullptr;

		// 设备变化(后端切换/重建)后旧句柄全部失效:整体丢弃,按新设备重建。
		if (m_Device != Renderer::GetDevice().get())
		{
			m_Entries.clear();
			m_Samplers.clear();
			m_Device = Renderer::GetDevice().get();
		}

		const std::string normalized = path.empty() ? std::string() : MaterialLibrary::NormalizePath(path);
		const std::string key = (srgb ? "s:" : "l:") + normalized;
		const auto cached = m_Entries.find(key);
		if (cached != m_Entries.end())
			return cached->second.Texture;

		Entry entry;
		if (normalized.empty())
		{
			entry.Texture = CreateWhite();
		}
		else
		{
			entry.Texture = Load(normalized, srgb, entry);
			if (!entry.Texture)
			{
				// 加载失败 ⇒ 1x1 白兜底(不保留半截产物元信息,避免查询到不存在的产物)。
				entry = Entry {};
				entry.Texture = CreateWhite();
			}
		}
		const Rhi::Handle<Rhi::Texture> texture = entry.Texture;
		entry.Valid = texture != nullptr;
		m_Entries.emplace(key, std::move(entry));
		return texture;
	}
}
