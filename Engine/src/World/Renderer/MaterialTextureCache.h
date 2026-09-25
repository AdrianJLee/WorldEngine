#pragma once

#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"
#include "World/Renderer/TextureArtifact.h"
#include "World/Renderer/TextureImportSettings.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace World
{
	// D3:材质贴图的 GPU 缓存(RHI 原生上传,双后端同一路径)。
	//
	//  - 键 = (规范化路径, 是否 sRGB 解码):同一文件以两种色彩空间使用时互不串味;
	//  - 缺图/坏图返回共享 1x1 白纹理(路径记在缓存里,不会每次重试加载);
	//  - 设备重建(后端切换)后调用 Clear(),句柄随旧设备一起失效。
	//
	// M4-TEX P2b:产物命中时同时记住产物头里的采样状态(wrap/filter/anisotropy)与格式,
	// 供渲染侧取 per-texture 采样器、判断法线贴图是否需要 BC5 重建 Z;两者都随设备重建失效。
	class WLD_API MaterialTextureCache
	{
	public:
		static MaterialTextureCache& Get();
		static void Shutdown();

		// srgb = albedo/emissive 类贴图(硬件解码到线性);normal/粗糙度等用 false(线性数据)。
		Rhi::Handle<Rhi::Texture> Get(const std::string& path, bool srgb);
		void Clear();

		// W5-L1:按路径失效(该路径的 sRGB/线性两份都清,空路径拒绝)。
		// 旧句柄交给 Renderer::QueueRelease 延迟释放;之后第一次 Get() 会重新读盘上传,
		// 因此"先失败兜底成 1x1 白纹理 → 文件后来变好"也能被这次失效救回来。
		void Invalidate(const std::string& path);

		// M4-TEX P2b:该逻辑路径当前是否由 BC5 产物提供(引擎标准着色器据此重建法线 Z)。
		// 回退(stb)路径 / 尚未加载过该路径 / 设备不匹配 ⇒ false —— 此时描述符也还没写过,
		// 同一帧的"标志 + 采样器"口径保持一致(渲染侧本来就有一帧描述符滞后)。
		bool IsBc5Artifact(const std::string& path, bool srgb) const;

		// M4-TEX P2b:该路径的产物采样器(wrap/filter/anisotropy 来自产物头,已按设备上限 clamp)。
		// 仅**产物命中**时返回有效句柄;没有产物 / 回退 stb / 创建失败 ⇒ nullptr,调用方继续用
		// 渲染器的共享 sampler(老资产行为逐字节不变)。采样器按 (wrap,filter,mip,aniso) 去重缓存。
		Rhi::Handle<Rhi::Sampler> GetSampler(const std::string& path, bool srgb);

	private:
		MaterialTextureCache() = default;

		struct Entry
		{
			Rhi::Handle<Rhi::Texture> Texture;
			bool Valid = true;
			// 产物元信息:FromArtifact=false 时其余字段无效(该路径走 stb 回退)。
			bool FromArtifact = false;
			TextureBlockFormat Format = TextureBlockFormat::Rgba8;
			TextureWrap Wrap = TextureWrap::Repeat;
			TextureFilter Filter = TextureFilter::Trilinear;
			uint32_t RequestedAnisotropy = 1;   // 产物头里的请求值(clamp 前的证据)
			Rhi::SamplerDesc Sampler;           // 已按设备上限 clamp 的采样状态
		};

		Rhi::Handle<Rhi::Texture> CreateWhite();
		Rhi::Handle<Rhi::Texture> Load(const std::string& normalizedPath, bool srgb, Entry& out);

		std::unordered_map<std::string, Entry> m_Entries;
		std::unordered_map<std::string, Rhi::Handle<Rhi::Sampler>> m_Samplers;
		void* m_Device = nullptr;   // 记录缓存所属设备指针,设备变化时整体失效
	};
}
