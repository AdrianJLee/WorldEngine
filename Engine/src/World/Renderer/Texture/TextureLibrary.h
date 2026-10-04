#pragma once

#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"
#include "World/Core/StringPool.h"
#include "World/Core/Thread/Thread.h"
#include "World/Renderer/Texture/TextureData.h"
#include "World/Renderer/Texture/TextureArtifact.h"
#include "World/Renderer/Texture/TextureImportSettings.h"

#include <cstdint>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace World
{
	// 全引擎**唯一**的 GPU 纹理驻留(RHI 原生,双后端同一路径;2026-10-04 收口)。
	//
	// 收口前这里只有"材质贴图",另有两处平行的驻留:
	//   * `Renderer2D` 的 GL 侧纹理(2D 精灵);
	//   * `BindUI` 的 `UiTextureCache`(脚本 ui.image)。
	// 同一张贴图被材质与精灵同时引用时会**解码两份、显存两份**;更糟的是 GL 那条路在 Vulkan 下
	// 要靠 `glGetTexImage` 读回整张图再重传(逐帧发生)。收口后 GL 纹理栈整体退役:
	// 一次解码、一份显存、一个键,并且**异步解码在工作线程、上传在主线程**。
	//
	//  - 键 = (驻留路径 id, 是否 sRGB 解码):同一文件以两种色彩空间使用时互不串味;
	//  - 缺图/坏图返回共享 1x1 白纹理(路径记在缓存里,不会每次重试加载);
	//  - 设备重建(后端切换)后调用 Clear(),句柄随旧设备一起失效。
	//
	// M4-TEX P2b:产物命中时同时记住产物头里的采样状态(wrap/filter/anisotropy)与格式,
	// 供渲染侧取 per-texture 采样器、判断法线贴图是否需要 BC5 重建 Z;两者都随设备重建失效。
	class WLD_API TextureLibrary
	{
	public:
		static TextureLibrary& Get();
		static void Shutdown();

		// 取 GPU 纹理。srgb = albedo/emissive 类贴图(硬件解码到线性);normal/粗糙度等用 false。
		// 空 id / 缺图 / 坏图 ⇒ 共享 1x1 白纹理(路径记在表里,不逐帧重试)。
		// **必须主线程**(内部走 device->CreateTexture)。
		Rhi::Handle<Rhi::Texture> Get(PathId path, bool srgb);
		// 逻辑路径重载(先驻留再委托):调用方持有字符串时用这个,与 Mesh::LoadWModel 同款双入口。
		Rhi::Handle<Rhi::Texture> Get(const std::string& logicalPath, bool srgb);
		void Clear();

		// W5-L1:按路径失效(该路径的 sRGB/线性两份都清)。
		// 旧句柄交给 Renderer::QueueRelease 延迟释放;之后第一次 Get() 会重新读盘上传,
		// 因此"先失败兜底成 1x1 白纹理 → 文件后来变好"也能被这次失效救回来。
		void Invalidate(PathId path);
		void Invalidate(const std::string& logicalPath);

		// ---- T5c:异步加载(与 AssetLoader 同一两阶段契约)----
		//
		// 切分与网格同源:工作线程做 **A 读盘 + B 解码**(`LoadTextureAsset` 读 `.wtexc` 产物
		// 或 stb 解码源图 —— 大贴图解码正是会卡住主线程的那一段);主线程做 **C 上传**
		// (`CreateTexture` / `SetDataMips`)。
		//
		// 取舍(明确记账):产物命中时工作线程**不**预解码回退源图 —— "产物在但建不出纹理"
		// 是罕见路径(设备不吃 BC / mip 链坏),那条路仍在主线程同步解码,不为此在常态下
		// 多付一次全图解码。
		void RequestAsync(PathId path, bool srgb);
		void RequestAsync(const std::string& logicalPath, bool srgb);
		// **主线程提交点**:把已完成项上传成 GPU 纹理并入表。返回本次真正提交的条数。
		std::size_t PumpCompletions();
		std::size_t PendingCount() const;
		// 加载进度摘要("loaded=N pending=M failed=K"),供加载界面/诊断。
		std::string DescribeLoads() const;

		// A+B 段的结果(纯 CPU):工作线程算好、主线程上传。公开是因为匿名命名空间里的
		// `DecodeTextureNow` 要构造/返回它(纯数据结构,暴露它不增加可调用面)。
		struct AsyncResult
		{
			bool Srgb = false;
			bool UseSource = false;   // true = 走解码后的像素;false = 走产物
			TextureAsset Asset;       // 产物命中:Header + Bytes(MipData 按偏移重算,故可安全搬运)
			TextureData Source;       // 无产物:已解码的 RGBA8 像素
			bool Ok = false;
			std::string Error;
		};

		// M4-TEX P2b:该逻辑路径当前是否由 BC5 产物提供(引擎标准着色器据此重建法线 Z)。
		// 回退(stb)路径 / 尚未加载过该路径 / 设备不匹配 ⇒ false —— 此时描述符也还没写过,
		// 同一帧的"标志 + 采样器"口径保持一致(渲染侧本来就有一帧描述符滞后)。
		bool IsBc5Artifact(PathId path, bool srgb) const;
		bool IsBc5Artifact(const std::string& logicalPath, bool srgb) const;

		// M4-TEX P2b:该路径的产物采样器(wrap/filter/anisotropy 来自产物头,已按设备上限 clamp)。
		// 仅**产物命中**时返回有效句柄;没有产物 / 回退 stb / 创建失败 ⇒ nullptr,调用方继续用
		// 渲染器的共享 sampler(老资产行为逐字节不变)。采样器按 (wrap,filter,mip,aniso) 去重缓存。
		Rhi::Handle<Rhi::Sampler> GetSampler(PathId path, bool srgb);
		Rhi::Handle<Rhi::Sampler> GetSampler(const std::string& logicalPath, bool srgb);

	private:
		TextureLibrary() = default;

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
		Rhi::Handle<Rhi::Texture> Load(const std::string& logicalPath, bool srgb, Entry& out);
		// 表的键 = (PathId, srgb) 打包成一个整数:uint64 = (id << 1) | srgb。
		static std::uint64_t MakeKey(PathId path, bool srgb)
		{
			return (static_cast<std::uint64_t>(path.Value) << 1) | (srgb ? 1ull : 0ull);
		}

		std::unordered_map<std::uint64_t, Entry> m_Entries;
		std::unordered_map<std::string, Rhi::Handle<Rhi::Sampler>> m_Samplers;

		void Publish(PathId path, bool srgb, AsyncResult result);
		Rhi::Handle<Rhi::Texture> Upload(PathId path, AsyncResult& result, Entry& out);

		mutable std::mutex m_AsyncMutex;
		std::unordered_map<std::uint64_t, AsyncResult> m_AsyncResults;
		std::vector<PathId> m_AsyncReady;    // 稳定顺序
		std::unordered_set<std::uint64_t> m_AsyncPending;
		JobCounter m_InFlight;
		void* m_Device = nullptr;   // 记录缓存所属设备指针,设备变化时整体失效
	};
}
