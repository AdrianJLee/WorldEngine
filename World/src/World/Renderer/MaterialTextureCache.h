#pragma once

#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"

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
	class WLD_API MaterialTextureCache
	{
	public:
		static MaterialTextureCache& Get();
		static void Shutdown();

		// srgb = albedo/emissive 类贴图(硬件解码到线性);normal/粗糙度等用 false(线性数据)。
		Rhi::Handle<Rhi::Texture> Get(const std::string& path, bool srgb);
		void Clear();

	private:
		MaterialTextureCache() = default;

		Rhi::Handle<Rhi::Texture> CreateWhite();
		Rhi::Handle<Rhi::Texture> Load(const std::string& normalizedPath, bool srgb);

		struct Entry
		{
			Rhi::Handle<Rhi::Texture> Texture;
			bool Valid = true;
		};
		std::unordered_map<std::string, Entry> m_Entries;
		void* m_Device = nullptr;   // 记录缓存所属设备指针,设备变化时整体失效
	};
}
