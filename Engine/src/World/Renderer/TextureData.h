#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	// 纹理 CPU 数据加载的**唯一入口**:VFS 优先 → 磁盘回退 → 1x1 白色兜底。
	// D3 把这段逻辑从 OpenGLTexture 抽出来,让 RHI 原生上传(3D 材质贴图)与
	// GL 纹理(2D/WUI)共用同一份查找与兜底策略,行为不会两边漂移。
	struct WLD_API TextureData
	{
		std::vector<uint8_t> Pixels;   // RGBA8,行序:第 0 行 = 图片顶部(未翻转)
		uint32_t Width = 1;
		uint32_t Height = 1;
		int Channels = 4;              // 原图通道数(1/2/3/4),供 GL 侧选择内部格式
		bool Valid = true;             // false = 文件缺失/损坏,内容为白色兜底并已打印警告
	};

	// flipVertically:GL 侧历史行为是 stbi_set_flip_vertically_on_load(1),
	// 即"上传后 UV (0,0) 在左下";RHI 原生的 3D 贴图按 Vulkan/GL 共同的左上原点约定,
	// 由调用方显式选择,避免依赖全局 stb 状态。
	WLD_API TextureData LoadTextureData(const std::string& path, bool flipVertically = false);
}
